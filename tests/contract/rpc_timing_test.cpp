// rpc_timing_test.cpp：RPC 排队/取消/关停时序的可控回归（G3）。
//
// 用可控 Registry（能阻塞 Query）+ 计数 Requester 固定时序，避免依赖真实
// 服务端的 sleep 竞猜：
//   queued_deadline_settles_on_time  前一条卡在发现阶段，后一条按自己的
//                                    截止时间兑现 kTimeout，且未触碰传输
//   cancel_before_commit_no_send     发现期间取消获胜 → 后端发送次数为 0
//   shutdown_releases_inflight       关停唤醒在途 I/O，每条请求只兑现一次
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/interfaces.h"
#include "core/rpc_frame.h"
#include "firefly/common/types.h"
#include "firefly/detail/endpoint_api.h"

using namespace std::chrono_literals;
using firefly::ErrorCode;
using firefly::detail::ClientHandle;
using firefly::detail::Deadline;
using firefly::detail::EndpointRecord;
using firefly::detail::EndpointRecordId;
using firefly::detail::IEndpointFactory;
using firefly::detail::IRegistry;
using firefly::detail::InjectionContext;
using firefly::detail::OnSettledFunc;
using firefly::detail::Publisher;
using firefly::detail::RegistryKind;
using firefly::detail::RpcRequester;
using firefly::detail::RpcResponder;
using firefly::detail::Subscriber;
using firefly::detail::TransferStatus;
using firefly::detail::WatchCallback;

namespace {

int Check(bool cond, int line) { return cond ? 0 : line; }

// 可控注册中心：Query 可在 Block() 后挂住，用来把执行者卡在"发现阶段"。
class BlockingRegistry final : public IRegistry {
 public:
  void Block() {
    std::lock_guard<std::mutex> lock(mutex_);
    blocked_ = true;
  }

  void Release() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      blocked_ = false;
    }
    cv_.notify_all();
  }

  int Queries() const { return queries_.load(); }

  bool WaitQueries(int expected, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (Queries() < expected) {
      if (std::chrono::steady_clock::now() >= deadline) return false;
      std::this_thread::sleep_for(1ms);
    }
    return true;
  }

  EndpointRecordId Register(const EndpointRecord&) override { return 1; }
  bool Unregister(EndpointRecordId) override { return true; }

  std::vector<EndpointRecord> Query(RegistryKind kind, const std::string& name,
                                    const std::string&) override {
    std::unique_lock<std::mutex> lock(mutex_);
    queries_.fetch_add(1);
    if (blocked_) cv_.wait(lock, [this] { return !blocked_; });
    EndpointRecord rec;
    rec.id = 1;
    rec.kind = kind;
    rec.name = name;
    rec.backend = "fake";
    rec.address = "fake://service";
    return {rec};
  }

  bool Has(RegistryKind, const std::string&) override { return true; }
  int Watch(RegistryKind, const std::string&, WatchCallback&&) override { return 1; }
  void Unwatch(int) override {}
  void Clear() override {}

 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool blocked_ = false;  // 受 mutex_ 保护
  std::atomic<int> queries_{0};
};

// 计数 Requester：记录进入 Exchange 的次数，用来断言"发送为零"。
class CountingRequester final : public RpcRequester {
 public:
  enum class Mode { kImmediate, kBlockUntilClosed };

  explicit CountingRequester(Mode mode) : mode_(mode) {}
  ~CountingRequester() override { Close(); }

  TransferStatus Exchange(const std::string&, const std::string& request,
                          Deadline, std::string* response) override {
    sends_.fetch_add(1);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      entered_ = true;
    }
    cv_.notify_all();
    if (mode_ == Mode::kBlockUntilClosed) {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this] { return closed_; });
      return TransferStatus::kClosed;
    }
    // 回包载荷回填请求字节，便于断言拿到的不是别人的响应。
    *response = firefly::detail::EncodeResponse(ErrorCode::kOk, request);
    return TransferStatus::kOk;
  }

  void Close() override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (closed_) return;
      closed_ = true;
    }
    cv_.notify_all();
  }

  bool WaitEntered(std::chrono::milliseconds limit) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, limit, [this] { return entered_ || closed_; });
  }

  bool closed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
  }

  int sends() const { return sends_.load(); }

 private:
  Mode mode_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool entered_ = false;  // 受 mutex_ 保护
  bool closed_ = false;   // 受 mutex_ 保护
  std::atomic<int> sends_{0};
};

class SingleRequesterFactory final : public IEndpointFactory {
 public:
  explicit SingleRequesterFactory(std::unique_ptr<RpcRequester> requester)
      : requester_(std::move(requester)) {}

  std::unique_ptr<Publisher> CreatePublisher() override { return nullptr; }
  std::unique_ptr<Subscriber> CreateSubscriber() override { return nullptr; }
  std::unique_ptr<RpcRequester> CreateRequester() override {
    return std::move(requester_);
  }
  std::unique_ptr<RpcResponder> CreateResponder() override { return nullptr; }

 private:
  std::unique_ptr<RpcRequester> requester_;
};

// 兑现观测点：记录首次兑现码、兑现次数，并识别重复兑现。
class Probe {
 public:
  Probe() { future_ = promise_.get_future(); }

  OnSettledFunc Callback() {
    return [this](ErrorCode code, const std::string&) {
      if (settles_.fetch_add(1) == 0) {
        promise_.set_value(code);
      } else {
        double_settle_.store(true);
      }
    };
  }

  bool Wait(std::chrono::milliseconds limit) {
    return future_.wait_for(limit) == std::future_status::ready;
  }

  ErrorCode Code() { return future_.get(); }
  int Settles() const { return settles_.load(); }
  bool DoubleSettled() const { return double_settle_.load(); }

 private:
  std::promise<ErrorCode> promise_;
  std::future<ErrorCode> future_;
  std::atomic<int> settles_{0};
  std::atomic<bool> double_settle_{false};
};

std::shared_ptr<InjectionContext> MakeContext(
    std::shared_ptr<IRegistry> registry, std::unique_ptr<RpcRequester> requester) {
  auto ctx = std::make_shared<InjectionContext>();
  ctx->endpoints =
      std::make_shared<SingleRequesterFactory>(std::move(requester));
  ctx->registry = std::move(registry);
  return ctx;
}

// 前一条请求卡在发现阶段时，排队请求必须按自己的截止时间兑现。
int TestQueuedDeadlineSettlesOnTime() {
  auto registry = std::make_shared<BlockingRegistry>();
  auto requester =
      std::make_unique<CountingRequester>(CountingRequester::Mode::kImmediate);
  CountingRequester* counter = requester.get();
  auto core = firefly::detail::CreateClientCore(
      "timing_node", "timing_svc", MakeContext(registry, std::move(requester)));
  if (!core) return 10;

  registry->Block();
  Probe blocked;
  const auto blocked_id =
      firefly::detail::ClientAsyncSendBytes(core.value.get(), "first", 5s,
                                            blocked.Callback());
  if (int e = Check(blocked_id != 0, 11)) return e;
  if (int e = Check(registry->WaitQueries(1, 2s), 12)) return e;  // 执行者已卡在发现

  Probe queued;
  const auto started = std::chrono::steady_clock::now();
  const auto queued_id = firefly::detail::ClientAsyncSendBytes(
      core.value.get(), "second", 100ms, queued.Callback());
  if (int e = Check(queued_id != 0, 13)) return e;

  // 到期线程按 deadline 兑现，不依赖执行者是否空闲。
  if (int e = Check(queued.Wait(500ms), 14)) return e;
  if (int e = Check(queued.Code() == ErrorCode::kTimeout, 15)) return e;
  const auto waited = std::chrono::steady_clock::now() - started;
  if (int e = Check(waited < 600ms, 16)) return e;
  // 过期请求从未进入传输层。
  if (int e = Check(counter->sends() == 0, 17)) return e;

  registry->Release();
  if (int e = Check(blocked.Wait(2s), 18)) return e;
  if (int e = Check(blocked.Code() == ErrorCode::kOk, 19)) return e;
  if (int e = Check(counter->sends() == 1, 20)) return e;
  if (int e = Check(queued.Settles() == 1, 21)) return e;

  firefly::detail::ClientShutdownCore(core.value.get());
  return 0;
}

// 发现阶段（提交点之前）取消获胜：后端发送次数必须为 0。
int TestCancelBeforeCommitNoSend() {
  auto registry = std::make_shared<BlockingRegistry>();
  auto requester =
      std::make_unique<CountingRequester>(CountingRequester::Mode::kImmediate);
  CountingRequester* counter = requester.get();
  auto core = firefly::detail::CreateClientCore(
      "timing_node", "timing_svc", MakeContext(registry, std::move(requester)));
  if (!core) return 30;

  registry->Block();
  Probe probe;
  const auto id = firefly::detail::ClientAsyncSendBytes(core.value.get(), "req",
                                                        5s, probe.Callback());
  if (int e = Check(id != 0, 31)) return e;
  if (int e = Check(registry->WaitQueries(1, 2s), 32)) return e;  // 已进入发现查询

  // 查询尚未返回：此时取消必须阻止发送。
  if (int e = Check(firefly::detail::ClientCancelRequestCore(core.value.get(), id),
                    33)) {
    return e;
  }
  if (int e = Check(probe.Wait(1s), 34)) return e;
  if (int e = Check(probe.Code() == ErrorCode::kCancelled, 35)) return e;

  registry->Release();  // 放行查询，执行者随后走到提交点
  std::this_thread::sleep_for(200ms);
  if (int e = Check(counter->sends() == 0, 36)) return e;
  if (int e = Check(probe.Settles() == 1, 37)) return e;
  // 已兑现的请求再次取消返回 false。
  if (int e = Check(!firefly::detail::ClientCancelRequestCore(core.value.get(), id),
                    38)) {
    return e;
  }

  firefly::detail::ClientShutdownCore(core.value.get());
  return 0;
}

// 关停：唤醒在途 I/O，排队与在途请求各兑现一次，且不必等到 deadline。
int TestShutdownReleasesInflight() {
  auto registry = std::make_shared<BlockingRegistry>();
  auto requester = std::make_unique<CountingRequester>(
      CountingRequester::Mode::kBlockUntilClosed);
  CountingRequester* counter = requester.get();
  auto core = firefly::detail::CreateClientCore(
      "timing_node", "timing_svc", MakeContext(registry, std::move(requester)));
  if (!core) return 40;

  Probe in_flight;
  const auto first = firefly::detail::ClientAsyncSendBytes(
      core.value.get(), "inflight", 10s, in_flight.Callback());
  if (int e = Check(first != 0, 41)) return e;
  if (int e = Check(counter->WaitEntered(2s), 42)) return e;  // 已提交，卡在 Exchange
  if (int e = Check(counter->sends() == 1, 43)) return e;

  Probe queued;
  const auto second = firefly::detail::ClientAsyncSendBytes(
      core.value.get(), "queued", 10s, queued.Callback());
  if (int e = Check(second != 0, 44)) return e;

  const auto started = std::chrono::steady_clock::now();
  firefly::detail::ClientShutdownCore(core.value.get());
  const auto elapsed = std::chrono::steady_clock::now() - started;
  // 关停关闭适配器唤醒在途 I/O，而不是等 10s 预算到期。
  if (int e = Check(elapsed < 1s, 45)) return e;
  if (int e = Check(counter->closed(), 46)) return e;

  if (int e = Check(in_flight.Wait(1s), 47)) return e;
  if (int e = Check(in_flight.Code() == ErrorCode::kCancelled, 48)) return e;
  if (int e = Check(in_flight.Settles() == 1, 49)) return e;
  if (int e = Check(queued.Wait(1s), 50)) return e;
  if (int e = Check(queued.Code() == ErrorCode::kCancelled, 51)) return e;
  if (int e = Check(queued.Settles() == 1, 52)) return e;
  if (int e = Check(!in_flight.DoubleSettled() && !queued.DoubleSettled(), 53)) {
    return e;
  }
  return 0;
}

}  // namespace

int main() {
  struct Case {
    const char* name;
    int (*fn)();
  } cases[] = {
      {"queued_deadline_settles_on_time", TestQueuedDeadlineSettlesOnTime},
      {"cancel_before_commit_no_send", TestCancelBeforeCommitNoSend},
      {"shutdown_releases_inflight", TestShutdownReleasesInflight},
  };
  for (auto& c : cases) {
    if (int e = c.fn()) {
      std::printf("rpc_timing: %s FAILED at line %d\n", c.name, e);
      return e;
    }
  }
  std::printf("rpc_timing: all %zu cases passed\n",
              sizeof(cases) / sizeof(cases[0]));
  return 0;
}
