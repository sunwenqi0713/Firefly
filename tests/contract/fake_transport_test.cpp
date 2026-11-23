// fake_transport_test.cpp：注入 fake transport/registry 跑通 Reader 收发逻辑，
// 不依赖真实 nng socket（fake 计数器断言）。测试允许依赖 detail 层。
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/interfaces.h"
#include "firefly/node/node.h"
#include "test_messages.pb.h"

using firefly::CallbackResult;
using firefly::detail::Deadline;
using firefly::detail::DefaultEndpointFactory;
using firefly::detail::EndpointRecord;
using firefly::detail::EndpointRecordId;
using firefly::detail::IEndpointFactory;
using firefly::detail::IRegistry;
using firefly::detail::Publisher;
using firefly::detail::RegistryEvent;
using firefly::detail::RegistryKind;
using firefly::detail::Subscriber;
using firefly::detail::TransferStatus;
using firefly::detail::WatchCallback;
using firefly::test::Chatter;

namespace {

struct SharedBus {
  std::mutex mutex;
  std::condition_variable cv;
  std::condition_variable status_cv;
  std::mutex rpc_mutex;
  std::condition_variable rpc_cv;
  int rpc_waiting = 0;  // 受 rpc_mutex 保护
  std::deque<std::string> frames;
  int waiting = 0;  // 受 mutex 保护：用于测试确认 Receive 已进入等待
  std::atomic<int> sends{0};
  std::atomic<int> recvs{0};
  std::atomic<int> dials{0};
  std::atomic<int> disconnects{0};
};

class FakePublisher final : public Publisher {
 public:
  explicit FakePublisher(std::shared_ptr<SharedBus> bus) : bus_(std::move(bus)) {}

  TransferStatus Bind(std::string* endpoint) override {
    if (!endpoint) return TransferStatus::kInvalidArgument;
    *endpoint = "fake://endpoint";
    return TransferStatus::kOk;
  }

  TransferStatus Send(const std::string& bytes) override {
    if (closed_.load(std::memory_order_acquire)) return TransferStatus::kClosed;
    {
      std::lock_guard<std::mutex> lock(bus_->mutex);
      bus_->frames.push_back(bytes);
    }
    bus_->sends.fetch_add(1);
    bus_->cv.notify_one();
    return TransferStatus::kOk;
  }

  void Close() override { closed_.store(true, std::memory_order_release); }

 private:
  std::shared_ptr<SharedBus> bus_;
  std::atomic<bool> closed_{false};
};

class FakeSubscriber final : public Subscriber {
 public:
  explicit FakeSubscriber(std::shared_ptr<SharedBus> bus) : bus_(std::move(bus)) {}

  TransferStatus UpdatePublishers(
      const std::vector<std::string>& endpoints) override {
    if (closed_.load(std::memory_order_acquire)) return TransferStatus::kClosed;
    std::lock_guard<std::mutex> lock(bus_->mutex);
    std::set<std::string> desired(endpoints.begin(), endpoints.end());
    for (const auto& url : connected_) {
      if (desired.count(url) == 0) bus_->disconnects.fetch_add(1);
    }
    for (const auto& url : desired) {
      if (!url.empty() && connected_.count(url) == 0) {
        connected_.insert(url);
        bus_->dials.fetch_add(1);
      }
    }
    connected_ = desired;
    connected_.erase("");
    return TransferStatus::kOk;
  }

  TransferStatus Receive(std::string* bytes, Deadline deadline) override {
    if (!bytes) return TransferStatus::kInvalidArgument;
    std::unique_lock<std::mutex> lock(bus_->mutex);
    for (;;) {
      if (closed_.load(std::memory_order_acquire)) return TransferStatus::kClosed;
      if (!bus_->frames.empty()) {
        *bytes = bus_->frames.front();
        bus_->frames.pop_front();
        bus_->recvs.fetch_add(1);
        return TransferStatus::kOk;
      }
      ++bus_->waiting;
      bus_->status_cv.notify_all();
      const bool woke_by_close = bus_->cv.wait_until(
          lock, deadline,
          [this] { return closed_.load(std::memory_order_acquire); });
      --bus_->waiting;
      bus_->status_cv.notify_all();
      if (woke_by_close) {
        return TransferStatus::kClosed;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        return TransferStatus::kTimeout;
      }
    }
  }

  void Close() override {
    closed_.store(true, std::memory_order_release);
    bus_->cv.notify_all();
  }

 private:
  std::shared_ptr<SharedBus> bus_;
  std::set<std::string> connected_;  // 受 bus_->mutex 保护
  std::atomic<bool> closed_{false};
};

class FakeReplyToken final : public firefly::detail::ReplyToken {
 public:
  explicit FakeReplyToken(const void* owner) : owner_(owner) {}
  const void* Owner() const noexcept { return owner_; }

 private:
  const void* owner_;
};

class FakeRequester final : public firefly::detail::RpcRequester {
 public:
  explicit FakeRequester(std::shared_ptr<SharedBus> bus) : bus_(std::move(bus)) {}

  TransferStatus Exchange(const std::string&, const std::string&,
                          Deadline deadline, std::string*) override {
    std::unique_lock<std::mutex> lock(bus_->rpc_mutex);
    ++bus_->rpc_waiting;
    bus_->rpc_cv.notify_all();
    const bool woke_by_close = bus_->rpc_cv.wait_until(
        lock, deadline,
        [this] { return closed_.load(std::memory_order_acquire); });
    --bus_->rpc_waiting;
    bus_->rpc_cv.notify_all();
    return woke_by_close ? TransferStatus::kClosed : TransferStatus::kTimeout;
  }

  void Close() override {
    closed_.store(true, std::memory_order_release);
    bus_->rpc_cv.notify_all();
  }

 private:
  std::shared_ptr<SharedBus> bus_;
  std::atomic<bool> closed_{false};
};

class FakeResponder final : public firefly::detail::RpcResponder {
 public:
  explicit FakeResponder(std::shared_ptr<SharedBus> bus) : bus_(std::move(bus)) {}

  TransferStatus Bind(std::string* endpoint) override {
    if (!endpoint) return TransferStatus::kInvalidArgument;
    *endpoint = "fake://rpc";
    return TransferStatus::kOk;
  }

  TransferStatus Receive(std::string*, std::unique_ptr<firefly::detail::ReplyToken>* token,
                         Deadline deadline) override {
    std::unique_lock<std::mutex> lock(bus_->rpc_mutex);
    const bool woke_by_close = bus_->rpc_cv.wait_until(
        lock, deadline,
        [this] { return closed_.load(std::memory_order_acquire); });
    if (woke_by_close) return TransferStatus::kClosed;
    if (token) *token = std::make_unique<FakeReplyToken>(this);
    return TransferStatus::kOk;
  }

  TransferStatus Reply(std::unique_ptr<firefly::detail::ReplyToken> token,
                       const std::string&, Deadline) override {
    auto* fake = dynamic_cast<FakeReplyToken*>(token.get());
    if (!fake || fake->Owner() != this) return TransferStatus::kInvalidArgument;
    return TransferStatus::kOk;
  }

  void Close() override {
    closed_.store(true, std::memory_order_release);
    bus_->rpc_cv.notify_all();
  }

 private:
  std::shared_ptr<SharedBus> bus_;
  std::atomic<bool> closed_{false};
};

class FakeEndpointFactory final : public IEndpointFactory {
 public:
  explicit FakeEndpointFactory(std::shared_ptr<SharedBus> bus) : bus_(std::move(bus)) {}
  std::unique_ptr<Publisher> CreatePublisher() override {
    return std::make_unique<FakePublisher>(bus_);
  }
  std::unique_ptr<Subscriber> CreateSubscriber() override {
    return std::make_unique<FakeSubscriber>(bus_);
  }
  std::unique_ptr<firefly::detail::RpcRequester> CreateRequester() override {
    return std::make_unique<FakeRequester>(bus_);
  }
  std::unique_ptr<firefly::detail::RpcResponder> CreateResponder() override {
    return std::make_unique<FakeResponder>(bus_);
  }

 private:
  std::shared_ptr<SharedBus> bus_;
};

class FakeRegistry final : public IRegistry {
 public:
  EndpointRecordId Register(const EndpointRecord& in) override {
    EndpointRecord rec = in;
    std::vector<WatchCallback> callbacks;
    std::vector<EndpointRecord> snapshot;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (rec.id == 0 || table_.count(rec.id) != 0) rec.id = next_record_id_++;
      if (rec.backend.empty()) rec.backend = "fake";
      table_[rec.id] = rec;
      snapshot = SnapshotLocked(rec.kind, rec.name);
      callbacks = WatchersLocked(rec.kind, rec.name);
    }
    Dispatch(callbacks, snapshot);
    return rec.id;
  }

  bool Unregister(EndpointRecordId id) override {
    std::vector<WatchCallback> callbacks;
    std::vector<EndpointRecord> snapshot;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = table_.find(id);
      if (it == table_.end()) return true;
      const auto kind = it->second.kind;
      const auto name = it->second.name;
      table_.erase(it);
      snapshot = SnapshotLocked(kind, name);
      callbacks = WatchersLocked(kind, name);
    }
    Dispatch(callbacks, snapshot);
    return true;
  }

  std::vector<EndpointRecord> Query(
      RegistryKind kind, const std::string& name,
      const std::string& backend = std::string()) override {
    std::lock_guard<std::mutex> lock(mutex_);
    auto all = SnapshotLocked(kind, name);
    if (backend.empty()) return all;
    std::vector<EndpointRecord> out;
    for (const auto& rec : all) {
      if (rec.backend == backend) out.push_back(rec);
    }
    return out;
  }

  bool Has(RegistryKind kind, const std::string& name) override {
    std::lock_guard<std::mutex> lock(mutex_);
    return !SnapshotLocked(kind, name).empty();
  }

  int Watch(RegistryKind kind, const std::string& name,
            WatchCallback&& cb) override {
    std::vector<EndpointRecord> snapshot;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const int id = next_watch_id_++;
      watches_.push_back(WatchEntry{id, kind, name, std::move(cb)});
      snapshot = SnapshotLocked(kind, name);
      Dispatch({watches_.back().cb}, snapshot);
      return id;
    }
  }

  void Unwatch(int id) override {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = watches_.begin(); it != watches_.end(); ++it) {
      if (it->id == id) {
        watches_.erase(it);
        return;
      }
    }
  }

  void Clear() override {
    std::lock_guard<std::mutex> lock(mutex_);
    table_.clear();
    watches_.clear();
  }

 private:
  struct WatchEntry {
    int id;
    RegistryKind kind;
    std::string name;
    WatchCallback cb;
  };

  std::vector<EndpointRecord> SnapshotLocked(RegistryKind kind,
                                             const std::string& name) const {
    std::vector<EndpointRecord> out;
    for (const auto& kv : table_) {
      if (kv.second.kind == kind && kv.second.name == name) {
        out.push_back(kv.second);
      }
    }
    return out;
  }

  std::vector<WatchCallback> WatchersLocked(RegistryKind kind,
                                            const std::string& name) const {
    std::vector<WatchCallback> out;
    for (const auto& w : watches_) {
      if (w.kind == kind && w.name == name) out.push_back(w.cb);
    }
    return out;
  }

  static void Dispatch(const std::vector<WatchCallback>& callbacks,
                       const std::vector<EndpointRecord>& snapshot) {
    for (const auto& cb : callbacks) {
      if (cb) cb(snapshot);
    }
  }

  std::mutex mutex_;
  std::map<EndpointRecordId, EndpointRecord> table_;
  std::vector<WatchEntry> watches_;
  EndpointRecordId next_record_id_ = 1;
  int next_watch_id_ = 1;
};

// 角色契约：Close() 幂等且能唤醒另一线程上的阻塞 I/O；Exchange 收绝对
// 截止时间并按期兑现，不依赖固定 sleep。
int TestRoleCloseAndDeadline() {
  auto bus = std::make_shared<SharedBus>();
  FakeEndpointFactory factory(bus);
  using firefly::detail::TransferStatus;
  using namespace std::chrono_literals;

  auto subscriber = factory.CreateSubscriber();
  std::promise<TransferStatus> recv_done;
  auto recv_future = recv_done.get_future();
  std::string bytes;
  std::thread recv_thread([&] {
    recv_done.set_value(subscriber->Receive(
        &bytes, std::chrono::steady_clock::now() + 10s));
  });
  {
    std::unique_lock<std::mutex> lock(bus->mutex);
    if (!bus->status_cv.wait_for(lock, 2s, [&] { return bus->waiting > 0; })) {
      return 100;
    }
  }
  subscriber->Close();
  subscriber->Close();  // 幂等
  if (recv_future.wait_for(2s) != std::future_status::ready) return 101;
  if (recv_future.get() != TransferStatus::kClosed) return 102;
  recv_thread.join();

  auto requester = factory.CreateRequester();
  std::promise<TransferStatus> exchange_done;
  auto exchange_future = exchange_done.get_future();
  std::thread exchange_thread([&] {
    std::string out;
    exchange_done.set_value(requester->Exchange(
        "fake://rpc", "request", std::chrono::steady_clock::now() + 50ms,
        &out));
  });
  {
    std::unique_lock<std::mutex> lock(bus->rpc_mutex);
    if (!bus->rpc_cv.wait_for(lock, 2s,
                              [&] { return bus->rpc_waiting > 0; })) {
      return 103;
    }
  }
  if (exchange_future.wait_for(2s) != std::future_status::ready) return 104;
  if (exchange_future.get() != TransferStatus::kTimeout) return 105;
  exchange_thread.join();

  auto requester2 = factory.CreateRequester();
  std::promise<TransferStatus> close_done;
  auto close_future = close_done.get_future();
  std::thread close_thread([&] {
    std::string out;
    close_done.set_value(requester2->Exchange(
        "fake://rpc", "request", std::chrono::steady_clock::now() + 10s, &out));
  });
  {
    std::unique_lock<std::mutex> lock(bus->rpc_mutex);
    if (!bus->rpc_cv.wait_for(lock, 2s,
                              [&] { return bus->rpc_waiting > 0; })) {
      return 106;
    }
  }
  requester2->Close();
  requester2->Close();
  if (close_future.wait_for(2s) != std::future_status::ready) return 107;
  if (close_future.get() != TransferStatus::kClosed) return 108;
  close_thread.join();
  return 0;
}

// 真实 NNG 适配器：同一契约在默认后端上也成立（绝对期限 / Close 唤醒）。
int TestRealAdapterDeadlineAndClose() {
  auto factory = DefaultEndpointFactory();
  using firefly::detail::TransferStatus;
  using namespace std::chrono_literals;

  auto subscriber = factory->CreateSubscriber();
  std::string bytes;
  const auto started = std::chrono::steady_clock::now();
  if (subscriber->Receive(&bytes, started + 50ms) !=
      TransferStatus::kTimeout) {
    return 110;
  }
  if (std::chrono::steady_clock::now() - started > 2s) return 111;

  auto subscriber2 = factory->CreateSubscriber();
  std::promise<void> entered;
  auto entered_future = entered.get_future();
  std::promise<TransferStatus> done;
  auto done_future = done.get_future();
  std::thread thread([&] {
    std::string out;
    entered.set_value();
    done.set_value(
        subscriber2->Receive(&out, std::chrono::steady_clock::now() + 10s));
  });
  if (entered_future.wait_for(2s) != std::future_status::ready) return 112;
  subscriber2->Close();
  subscriber2->Close();
  if (done_future.wait_for(2s) != std::future_status::ready) return 113;
  if (done_future.get() != TransferStatus::kClosed) return 114;
  thread.join();
  return 0;
}

}  // namespace

int main() {
  firefly::Init("fake_transport_test");
  auto bus = std::make_shared<SharedBus>();
  auto ctx = std::make_shared<firefly::detail::InjectionContext>();
  ctx->endpoints = std::make_shared<FakeEndpointFactory>(bus);
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

  if (int e = TestRoleCloseAndDeadline(); e != 0) return e;
  if (int e = TestRealAdapterDeadlineAndClose(); e != 0) return e;

  firefly::Clear();
  return 0;
}
