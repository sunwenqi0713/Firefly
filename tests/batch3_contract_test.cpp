// batch3_contract_test.cpp：第 3 批发现层契约（记录模型 / 生命周期 / 注册失败）。
// 并发时序由 promise/future 控制；NNG 后端下每个用例使用独立的发现中心地址，
// 内存后端不需要网络地址。
#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "firefly/detail/interfaces.h"
#if FIREFLY_WITH_NNG
#include <nng/nng.h>
#include <nng/protocol/pubsub0/pub.h>
#include "firefly/detail/net.h"
#endif
#include "firefly/firefly.h"
#include "test_messages.pb.h"

using namespace std::chrono_literals;
using firefly::CallbackResult;
using firefly::ErrorCode;
using firefly::WriteStatus;
using firefly::detail::DefaultRegistry;
using firefly::detail::EndpointRecord;
using firefly::detail::IEndpointFactory;
using firefly::detail::IRegistry;
using firefly::detail::Publisher;
using firefly::detail::RegistryKind;
using firefly::detail::RpcRequester;
using firefly::detail::RpcResponder;
using firefly::detail::Subscriber;
using firefly::detail::TransferStatus;
using firefly::test::CalcRequest;
using firefly::test::CalcResponse;
using firefly::test::Chatter;

namespace {

int Check(bool cond, int line) { return cond ? 0 : line; }

template <class Predicate>
bool WaitUntil(Predicate pred, std::chrono::milliseconds limit) {
  const auto deadline = std::chrono::steady_clock::now() + limit;
  while (!pred()) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::yield();
  }
  return true;
}

#if FIREFLY_WITH_NNG
// 用 NNG 临时监听端口取两个空闲端口，避免与固定 37221/37222 冲突。
std::pair<int, int> PickFreePorts() {
  std::vector<nng_socket> socks;
  std::vector<nng_listener> listeners;
  std::vector<std::string> urls;
  for (int i = 0; i < 2; ++i) {
    nng_socket sock = NNG_SOCKET_INITIALIZER;
    nng_listener listener = NNG_LISTENER_INITIALIZER;
    std::string url;
    if (nng_pub0_open(&sock) != 0 ||
        !firefly::detail::ListenOnEphemeralPort(sock, &listener, &url)) {
      for (auto s : socks) nng_close(s);
      return {};
    }
    socks.push_back(sock);
    listeners.push_back(listener);
    urls.push_back(url);
  }
  auto port_of = [](const std::string& url) {
    const auto pos = url.rfind(':');
    return pos == std::string::npos ? 0 : std::stoi(url.substr(pos + 1));
  };
  const auto result = std::make_pair(port_of(urls[0]), port_of(urls[1]));
  for (auto s : socks) nng_close(s);
  return result;
}
#endif

bool StartCase(const char* name) {
#if FIREFLY_WITH_NNG
  const auto ports = PickFreePorts();
  if (ports.first == 0 || ports.second == 0) return false;
  firefly::detail::ConfigureDiscovery(
      {"tcp://127.0.0.1:" + std::to_string(ports.first),
       "tcp://127.0.0.1:" + std::to_string(ports.second)});
#else
  (void)name;
#endif
  return firefly::Init(name);
}

Chatter MakeMsg(int seq) {
  Chatter msg;
  msg.set_seq(seq);
  msg.set_content("batch3");
  return msg;
}

// ---------- 两个同通道 Reader 分属不同 Node：删一个另一个仍收消息 ----------
int TestTwoReadersIsolatedDeletion() {
  if (!StartCase("b3_two_readers")) return 1;
  {
    auto node_a = firefly::CreateNode("b3_reader_node_a").value;
    auto node_b = firefly::CreateNode("b3_reader_node_b").value;
    const std::string channel = "b3/two_readers";
    auto writer = node_a->CreateWriter<Chatter>(channel).value;
    if (!writer) return 2;

    std::promise<void> got;
    auto got_future = got.get_future();
    std::atomic<bool> signaled{false};
    auto reader_b = node_b->CreateReader<Chatter>(
        channel, [&](const firefly::MessageContext&,
                     const std::shared_ptr<const Chatter>& msg) {
          if (msg->seq() == 77 && !signaled.exchange(true)) got.set_value();
          return CallbackResult::kOk;
        }).value;
    auto reader_a = node_a->CreateReader<Chatter>(channel).value;
    if (!reader_a || !reader_b) return 3;

    if (int e = Check(WaitUntil(
                          [&] {
                            return DefaultRegistry()
                                       ->Query(RegistryKind::kReader, channel)
                                       .size() == 2;
                          },
                          3s),
                      4)) {
      return e;
    }
    if (!node_a->DeleteReader(channel)) return 5;
    if (node_a->GetReader<Chatter>(channel) != nullptr) return 6;
    if (node_b->GetReader<Chatter>(channel) == nullptr) return 7;
    if (DefaultRegistry()->Query(RegistryKind::kReader, channel).size() != 1) {
      return 8;
    }

    for (int i = 0; i < 300 && got_future.wait_for(0ms) != std::future_status::ready; ++i) {
      writer->Write(MakeMsg(77));
      std::this_thread::yield();
    }
    if (int e = Check(got_future.wait_for(2s) == std::future_status::ready, 9)) {
      return e;
    }
  }
  firefly::Clear();
  return 0;
}

// ---------- Reader 启动期间新增 Writer 最终被发现 ----------
int TestWriterAddedAfterReaderStarts() {
  if (!StartCase("b3_late_writer")) return 20;
  {
    auto node = firefly::CreateNode("b3_late_node").value;
    const std::string channel = "b3/late_writer";
    std::promise<void> got;
    auto got_future = got.get_future();
    std::atomic<bool> signaled{false};
    auto reader = node->CreateReader<Chatter>(
        channel, [&](const firefly::MessageContext&,
                     const std::shared_ptr<const Chatter>& msg) {
          if (msg->seq() == 42 && !signaled.exchange(true)) got.set_value();
          return CallbackResult::kOk;
        }).value;
    if (!reader) return 21;
    if (int e = Check(WaitUntil(
                          [&] {
                            return !DefaultRegistry()
                                        ->Query(RegistryKind::kReader, channel)
                                        .empty();
                          },
                          3s),
                      22)) {
      return e;
    }
    auto writer = node->CreateWriter<Chatter>(channel).value;
    if (!writer) return 23;
    for (int i = 0; i < 300 && got_future.wait_for(0ms) != std::future_status::ready; ++i) {
      writer->Write(MakeMsg(42));
      std::this_thread::yield();
    }
    if (int e = Check(got_future.wait_for(2s) == std::future_status::ready, 24)) {
      return e;
    }
  }
  firefly::Clear();
  return 0;
}

// ---------- Clear / 销毁 / 再次 Init 后查不到旧记录 ----------
int TestClearAndReinitNoStaleRecords() {
  if (!StartCase("b3_clear_reinit")) return 30;
  const std::string channel = "b3/stale";
  {
    auto node = firefly::CreateNode("b3_stale_node").value;
    auto writer = node->CreateWriter<Chatter>(channel).value;
    if (!writer) return 31;
    if (DefaultRegistry()->Query(RegistryKind::kWriter, channel).empty()) {
      return 32;
    }
  }
  firefly::Clear();
  // Query 会惰性重启发现中心；重启后必须看不到旧记录。
  if (!DefaultRegistry()->Query(RegistryKind::kWriter, channel).empty()) {
    return 33;
  }
  firefly::Clear();
  if (!StartCase("b3_clear_reinit_2")) return 34;
  if (!DefaultRegistry()->Query(RegistryKind::kWriter, channel).empty()) {
    return 35;
  }
  firefly::Clear();
  return 0;
}

// ---------- 多记录 + backend 过滤 + 按 id 注销 ----------
int TestMultiRecordAndBackendFilter() {
  if (!StartCase("b3_backend_filter")) return 40;
  auto reg = DefaultRegistry();
  EndpointRecord a;
  a.kind = RegistryKind::kService;
  a.name = "b3/filter";
  a.address = "tcp://127.0.0.1:41001";
  a.backend = "nng";
  EndpointRecord b = a;
  b.address = "tcp://127.0.0.1:41002";
  b.backend = "fake";
  const auto id_a = reg->Register(a);
  const auto id_b = reg->Register(b);
  if (id_a == 0 || id_b == 0 || id_a == id_b) return 41;
  if (reg->Query(RegistryKind::kService, a.name).size() != 2) return 42;
  if (reg->Query(RegistryKind::kService, a.name, "nng").size() != 1) return 43;
  if (reg->Query(RegistryKind::kService, a.name, "fake").size() != 1) return 44;
  reg->Unregister(id_a);
  if (reg->Query(RegistryKind::kService, a.name, "nng").size() != 0) return 45;
  if (reg->Query(RegistryKind::kService, a.name, "fake").size() != 1) return 46;
  reg->Unregister(id_b);
  firefly::Clear();
  return 0;
}

// ---------- 注册失败：关闭已创建端点，不留工作线程/监听器 ----------
struct FailRegistry final : IRegistry {
  firefly::detail::EndpointRecordId Register(const EndpointRecord&) override {
    ++calls;
    return 0;
  }
  bool Unregister(firefly::detail::EndpointRecordId) override { return true; }
  std::vector<EndpointRecord> Query(RegistryKind, const std::string&,
                                    const std::string&) override {
    return {};
  }
  bool Has(RegistryKind, const std::string&) override { return false; }
  int Watch(RegistryKind, const std::string&,
            firefly::detail::WatchCallback&&) override {
    return 0;
  }
  void Unwatch(int) override {}
  void Clear() override {}
  int calls = 0;
};

struct CountingPublisher final : Publisher {
  explicit CountingPublisher(int* closes) : closes_(closes) {}
  TransferStatus Bind(std::string* endpoint) override {
    *endpoint = "fake://bind";
    return TransferStatus::kOk;
  }
  TransferStatus Send(const std::string&) override { return TransferStatus::kOk; }
  void Close() override { ++*closes_; }
  int* closes_;
};

struct CountingSubscriber final : Subscriber {
  explicit CountingSubscriber(int* closes, int* receives)
      : closes_(closes), receives_(receives) {}
  TransferStatus UpdatePublishers(
      const std::vector<std::string>&) override {
    return TransferStatus::kOk;
  }
  TransferStatus Receive(std::string*, firefly::detail::Deadline) override {
    ++*receives_;
    return TransferStatus::kTimeout;
  }
  void Close() override { ++*closes_; }
  int* closes_;
  int* receives_;
};

struct CountingResponder final : RpcResponder {
  explicit CountingResponder(int* closes) : closes_(closes) {}
  TransferStatus Bind(std::string* endpoint) override {
    *endpoint = "fake://rep";
    return TransferStatus::kOk;
  }
  TransferStatus Receive(std::string*, std::unique_ptr<firefly::detail::ReplyToken>*,
                         firefly::detail::Deadline) override {
    return TransferStatus::kTimeout;
  }
  TransferStatus Reply(std::unique_ptr<firefly::detail::ReplyToken>,
                       const std::string&, firefly::detail::Deadline) override {
    return TransferStatus::kOk;
  }
  void Close() override { ++*closes_; }
  int* closes_;
};

struct CountingRequester final : RpcRequester {
  TransferStatus Exchange(const std::string&, const std::string&,
                          firefly::detail::Deadline, std::string*) override {
    return TransferStatus::kTimeout;
  }
  void Close() override {}
};

struct CountingEndpoints final : IEndpointFactory {
  std::unique_ptr<Publisher> CreatePublisher() override {
    return std::make_unique<CountingPublisher>(&publisher_closes);
  }
  std::unique_ptr<Subscriber> CreateSubscriber() override {
    return std::make_unique<CountingSubscriber>(&subscriber_closes, &receives);
  }
  std::unique_ptr<RpcRequester> CreateRequester() override {
    return std::make_unique<CountingRequester>();
  }
  std::unique_ptr<RpcResponder> CreateResponder() override {
    return std::make_unique<CountingResponder>(&responder_closes);
  }
  int publisher_closes = 0;
  int subscriber_closes = 0;
  int responder_closes = 0;
  int receives = 0;
};

int TestRegisterFailureNoResidual() {
  if (!StartCase("b3_register_fail")) return 50;
  {
    auto ctx = std::make_shared<firefly::detail::InjectionContext>();
    auto endpoints = std::make_shared<CountingEndpoints>();
    auto reg = std::make_shared<FailRegistry>();
    ctx->endpoints = endpoints;
    ctx->registry = reg;
    auto node = firefly::CreateNode("b3_fail_node", "", ctx).value;
    if (!node) return 51;

    auto writer = node->CreateWriter<Chatter>("b3/fail");
    if (int e = Check(!writer && writer.code == ErrorCode::kTransport, 52)) return e;
    if (int e = Check(endpoints->publisher_closes == 1, 53)) return e;

    auto reader = node->CreateReader<Chatter>("b3/fail");
    if (int e = Check(!reader && reader.code == ErrorCode::kTransport, 54)) return e;
    if (int e = Check(endpoints->subscriber_closes == 1, 55)) return e;
    if (int e = Check(endpoints->receives == 0, 56)) return e;  // 未启动收线程

    auto service = node->CreateService<CalcRequest, CalcResponse>(
        "b3_fail_svc", [](const std::shared_ptr<const CalcRequest>& req) {
          auto r = std::make_shared<CalcResponse>();
          r->set_sum(req->a() + req->b());
          return firefly::ServiceResult<CalcResponse>{ErrorCode::kOk, r};
        });
    if (int e = Check(!service && service.code == ErrorCode::kTransport, 57)) return e;
    if (int e = Check(endpoints->responder_closes == 1, 58)) return e;
    if (int e = Check(reg->calls == 3, 59)) return e;
  }
  firefly::Clear();
  return 0;
}

}  // namespace

int main() {
  struct Case {
    const char* name;
    int (*fn)();
  } cases[] = {
      {"two_readers_isolated_deletion", TestTwoReadersIsolatedDeletion},
      {"writer_added_after_reader_starts", TestWriterAddedAfterReaderStarts},
      {"clear_and_reinit_no_stale_records", TestClearAndReinitNoStaleRecords},
      {"multi_record_and_backend_filter", TestMultiRecordAndBackendFilter},
      {"register_failure_no_residual", TestRegisterFailureNoResidual},
  };
  for (const auto& c : cases) {
    if (int e = c.fn()) {
      std::printf("batch3_contract: %s FAILED at line %d\n", c.name, e);
      return e;
    }
  }
  std::printf("batch3_contract: all %zu cases passed\n",
              sizeof(cases) / sizeof(cases[0]));
  return 0;
}
