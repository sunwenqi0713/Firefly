// fake_transport_test.cpp：注入 fake transport/registry 跑通 Reader 收发逻辑，
// 不依赖真实 nng socket（fake 计数器断言）。测试允许依赖 detail 层。
#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "firefly/detail/interfaces.h"
#include "firefly/node/node.h"
#include "test_messages.pb.h"

using firefly::CallbackResult;
using firefly::detail::IRegistry;
using firefly::detail::ISocket;
using firefly::detail::ITransportFactory;
using firefly::detail::RegistryEvent;
using firefly::detail::RegistryKind;
using firefly::detail::TransportKind;
using firefly::test::Chatter;

namespace {

struct SharedBus {
  std::mutex mutex;
  std::deque<std::string> frames;
  std::atomic<int> sends{0};
  std::atomic<int> recvs{0};
  std::atomic<int> dials{0};
};

class FakeSocket final : public ISocket {
 public:
  FakeSocket(TransportKind kind, std::shared_ptr<SharedBus> bus)
      : kind_(kind), bus_(std::move(bus)) {}

  TransportKind Kind() const override { return kind_; }

  int BindEphemeral(std::string* url) override {
    if (url) *url = "fake://endpoint";
    return 0;
  }

  int Dial(const std::string& url) override {
    (void)url;
    bus_->dials.fetch_add(1);
    return 0;
  }

  int Send(const std::string& bytes) override {
    {
      std::lock_guard<std::mutex> lock(bus_->mutex);
      bus_->frames.push_back(bytes);
    }
    bus_->sends.fetch_add(1);
    return 0;
  }

  int Recv(std::string* bytes, int timeout_ms) override {
    (void)timeout_ms;
    {
      std::lock_guard<std::mutex> lock(bus_->mutex);
      if (!bus_->frames.empty()) {
        *bytes = bus_->frames.front();
        bus_->frames.pop_front();
        bus_->recvs.fetch_add(1);
        return firefly::detail::kIoOk;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return firefly::detail::kIoTimeout;
  }

  int SetRecvTimeout(int) override { return 0; }
  int SetSendTimeout(int) override { return 0; }
  bool Valid() const override { return true; }
  void Close() override {}

 private:
  TransportKind kind_;
  std::shared_ptr<SharedBus> bus_;
};

class FakeFactory final : public ITransportFactory {
 public:
  explicit FakeFactory(std::shared_ptr<SharedBus> bus) : bus_(std::move(bus)) {}
  std::unique_ptr<ISocket> Create(TransportKind kind) override {
    return std::make_unique<FakeSocket>(kind, bus_);
  }

 private:
  std::shared_ptr<SharedBus> bus_;
};

class FakeRegistry final : public IRegistry {
 public:
  bool Register(RegistryKind kind, const std::string& name,
                const std::string& url) override {
    std::vector<std::function<void(const RegistryEvent&)>> callbacks;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      table_[{kind, name}].insert(url);
      for (auto& sub : subs_) {
        if (sub.kind == kind && sub.name == name) callbacks.push_back(sub.cb);
      }
    }
    auto endpoint = firefly::detail::MakeEndpointId(url);
    for (auto& cb : callbacks) {
      cb(RegistryEvent{kind, name, endpoint, true});
    }
    return true;
  }

  bool Unregister(RegistryKind kind, const std::string& name,
                  const std::string& url) override {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = table_.find({kind, name});
    if (it != table_.end()) it->second.erase(url);
    return true;
  }

  std::vector<std::string> List(RegistryKind kind,
                                const std::string& name) override {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = table_.find({kind, name});
    if (it == table_.end()) return {};
    return {it->second.begin(), it->second.end()};
  }

  bool Has(RegistryKind kind, const std::string& name) override {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = table_.find({kind, name});
    return it != table_.end() && !it->second.empty();
  }

  int Subscribe(RegistryKind kind, const std::string& name,
                std::function<void(const RegistryEvent&)>&& cb) override {
    std::lock_guard<std::mutex> lock(mutex_);
    const int id = next_id_++;
    subs_.push_back({id, kind, name, std::move(cb)});
    return id;
  }

  void Unsubscribe(int id) override {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = subs_.begin(); it != subs_.end(); ++it) {
      if (it->id == id) {
        subs_.erase(it);
        return;
      }
    }
  }

 private:
  struct Sub {
    int id;
    RegistryKind kind;
    std::string name;
    std::function<void(const RegistryEvent&)> cb;
  };
  std::mutex mutex_;
  std::map<std::pair<RegistryKind, std::string>, std::set<std::string>> table_;
  std::vector<Sub> subs_;
  int next_id_ = 1;
};

}  // namespace

int main() {
  firefly::Init("fake_transport_test");
  auto bus = std::make_shared<SharedBus>();
  auto ctx = std::make_shared<firefly::detail::InjectionContext>();
  ctx->transport = std::make_shared<FakeFactory>(bus);
  ctx->registry = std::make_shared<FakeRegistry>();

  auto node = firefly::CreateNode("fake_node", "", ctx).value;
  if (!node) return 1;

  // 先 Writer 再 Reader：Reader 创建时 List 能发现 writer（fake registry）
  auto writer = node->CreateWriter<Chatter>("fake_ch").value;
  if (!writer) return 2;

  std::atomic<int> received{0};
  std::atomic<long long> last_seq{-1};
  auto reader = node->CreateReader<Chatter>(
      "fake_ch",
      [&](const firefly::MessageContext& context,
          const std::shared_ptr<const Chatter>& msg) {
        if (context.channel != "fake_ch") return CallbackResult::kDropped;
        last_seq.store(msg->seq());
        received.fetch_add(1);
        return CallbackResult::kOk;
      }).value;
  if (!reader) return 3;
  if (bus->dials.load() < 1) return 4;  // 假传输收到 dial

  Chatter msg;
  msg.set_seq(77);
  msg.set_content("via-fake");
  if (writer->Write(msg) != firefly::WriteStatus::kAccepted) return 5;
  std::this_thread::sleep_for(std::chrono::milliseconds(400));

  if (bus->sends.load() != 1) return 6;
  if (bus->recvs.load() < 1) return 7;
  if (received.load() != 1 || last_seq.load() != 77) return 8;
  if (reader->Stats().recv != 1) return 9;

  firefly::Clear();
  return 0;
}
