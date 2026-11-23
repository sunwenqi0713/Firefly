#include "firefly/detail/endpoint_api.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "firefly/common/log.h"
#include "core/interfaces.h"
#include "core/rpc_frame.h"

namespace firefly {
namespace detail {
namespace {

// 有界请求队列：同步与异步共用，队列满时直接以现有错误码 kQueueFull 失败。
constexpr std::size_t kMaxQueuedRequests = 64;

ErrorCode MapExchangeStatus(TransferStatus status) noexcept {
  switch (status) {
    case TransferStatus::kTimeout:
      return ErrorCode::kTimeout;
    case TransferStatus::kClosed:
      return ErrorCode::kNotReady;
    case TransferStatus::kWouldBlock:
      return ErrorCode::kQueueFull;
    case TransferStatus::kIoError:
    case TransferStatus::kInvalidArgument:
    case TransferStatus::kOk:
      return ErrorCode::kTransport;
  }
  return ErrorCode::kTransport;
}

// 请求状态机（stage 与 on_settled 由同一把锁保护）：
//   kQueued --TryStartSending--> kSending --> kDone（Settle）
//   kQueued / kSending --Cancel--> kDone（立即兑现 kCancelled）
// 因此"取消与发送谁先发生"只有一次裁决，且每请求只兑现一次。
// 排队中取消：TryStartSending 返回 false，请求从发送路径移除，服务端 0 次。
// 已提交发送后取消：只结束本地等待，发送与接收继续进行，服务端仍可能执行。
struct PendingCall {
  enum class Stage { kQueued, kSending, kDone };

  explicit PendingCall(OnSettledFunc cb) : on_settled(std::move(cb)) {}

  // 执行者出队后、真正 Send 前调用。
  bool TryStartSending() {
    std::lock_guard<std::mutex> lock(state_mutex);
    if (stage != Stage::kQueued) return false;
    stage = Stage::kSending;
    return true;
  }

  // 返回 true 表示本次调用取得了兑现权；已兑现的请求再次 Cancel 返回 false。
  bool Cancel(OnSettledFunc* callback) {
    std::lock_guard<std::mutex> lock(state_mutex);
    if (stage == Stage::kDone) return false;
    stage = Stage::kDone;
    *callback = std::move(on_settled);
    return true;
  }

  void Settle(ErrorCode code, const std::string& payload) {
    OnSettledFunc callback;
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      if (stage == Stage::kDone) return;  // 迟到响应 / 重复兑现一律丢弃
      stage = Stage::kDone;
      callback = std::move(on_settled);
    }
    if (callback) callback(code, payload);
  }

 private:
  std::mutex state_mutex;
  Stage stage = Stage::kQueued;
  OnSettledFunc on_settled;
};

struct QueuedCall {
  std::uint64_t id = 0;
  std::string bytes;
  Deadline deadline{};
  std::shared_ptr<PendingCall> pending;
};

struct ClientCore final : ClientHandle {
  ClientCore(std::unique_ptr<RpcRequester> requester,
             std::shared_ptr<IRegistry> reg)
      : requester(std::move(requester)), registry(std::move(reg)) {}

  ~ClientCore() override {
    ShutdownInternal();
    // 仅当析构发生在 I/O 线程内（兑现回调释放最后引用）时无法 join 自身。
    if (io_thread_.joinable()) io_thread_.detach();
  }

  void StartIo() { io_thread_ = std::thread([this] { IoLoop(); }); }

  static int RemainingMs(Deadline deadline) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return 0;
    return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                deadline - now).count());
  }

  // 一次请求的 Core 侧 I/O 路径：服务查询与整体预算在 Core，连接/重连/
  // 发送/接收以及各步剩余预算在 RpcRequester 适配器内。PendingCall 的取消
  // 裁决仍在 IoLoop 中完成，适配器完全不感知取消状态。
  WireResponse Exchange(const std::string& bytes, Deadline deadline) {
    WireResponse out;
    if (shutdown_requested_.load(std::memory_order_acquire)) {
      out.code = ErrorCode::kNotReady;
      return out;
    }
    if (RemainingMs(deadline) <= 0) {
      out.code = ErrorCode::kTimeout;
      return out;
    }
    auto records = registry->Query(RegistryKind::kService, service_name);
    if (RemainingMs(deadline) <= 0) {
      out.code = ErrorCode::kTimeout;
      return out;
    }
    if (records.empty()) {
      out.code = ErrorCode::kServiceNotFound;
      return out;
    }
    if (shutdown_requested_.load(std::memory_order_acquire)) {
      out.code = ErrorCode::kNotReady;
      return out;
    }
    std::string frame;
    TransferStatus status =
        requester->Exchange(records.front().address, bytes, deadline, &frame);
    if (status != TransferStatus::kOk) {
      out.code = MapExchangeStatus(status);
      return out;
    }
    sent_count_.fetch_add(1, std::memory_order_relaxed);

    ErrorCode frame_code = ErrorCode::kSerialization;
    std::string payload;
    if (!DecodeResponse(frame, &frame_code, &payload)) {
      FY_WARN("client[%s]: malformed response frame (%zu bytes)",
              service_name.c_str(), frame.size());
      out.code = ErrorCode::kSerialization;
      return out;
    }
    out.code = frame_code;
    out.payload = std::move(payload);
    return out;
  }

  // 同步与异步唯一入队口。锁序恒为 pending_mutex_ → queue_mutex_，与
  // ShutdownInternal 一致；回调一律在锁外兑现。
  std::uint64_t Enqueue(const std::string& bytes,
                        std::chrono::milliseconds timeout,
                        OnSettledFunc on_settled) {
    auto pending = std::make_shared<PendingCall>(std::move(on_settled));
    const std::uint64_t id = next_id_.fetch_add(1);
    const Deadline deadline = std::chrono::steady_clock::now() + timeout;
    ErrorCode failure = ErrorCode::kOk;
    {
      std::lock_guard<std::mutex> pending_lock(pending_mutex_);
      if (shutdown_requested_.load(std::memory_order_acquire)) {
        failure = ErrorCode::kNotReady;
      } else {
        std::lock_guard<std::mutex> queue_lock(queue_mutex_);
        if (io_stopping_ || queue_.size() >= kMaxQueuedRequests) {
          failure = ErrorCode::kQueueFull;
        } else {
          queue_.push_back(QueuedCall{id, bytes, deadline, pending});
          pending_[id] = pending;
          ++in_flight_;
        }
      }
    }
    if (failure != ErrorCode::kOk) {
      pending->Settle(failure, "");
      return 0;
    }
    queue_cv_.notify_one();
    return id;
  }

  void IoLoop() {
    for (;;) {
      QueuedCall call;
      {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        queue_cv_.wait(lock, [this] { return io_stopping_ || !queue_.empty(); });
        if (queue_.empty()) break;  // 关停且队列已清空
        call = std::move(queue_.front());
        queue_.pop_front();
      }
      WireResponse out{ErrorCode::kCancelled, ""};
      // 唯一的"取消 vs 发送"裁决点：排队中已取消则不触碰连接。
      if (call.pending->TryStartSending()) {
        out = RemainingMs(call.deadline) <= 0
                  ? WireResponse{ErrorCode::kTimeout, ""}
                  : Exchange(call.bytes, call.deadline);
        if (out.code == ErrorCode::kCancelled) out.code = ErrorCode::kTimeout;
      }
      Finish(call.id, call.pending.get(), out);
    }
  }

  // 先退出 pending 表、递减计数，再兑现：兑现回调可安全调用 Cancel/Shutdown。
  void Finish(std::uint64_t id, PendingCall* pending, const WireResponse& out) {
    {
      std::lock_guard<std::mutex> lock(pending_mutex_);
      // Shutdown 可能已统一回收 pending 表；重复回收不得让计数下溢。
      if (pending_.erase(id) != 0 && in_flight_ > 0 && --in_flight_ == 0) {
        idle_cv_.notify_all();
      }
    }
    pending->Settle(out.code, out.payload);
  }

  void ShutdownInternal() {
    if (shutdown_done_.exchange(true)) return;
    shutdown_requested_.store(true, std::memory_order_release);
    std::vector<std::shared_ptr<PendingCall>> to_cancel;
    {
      std::lock_guard<std::mutex> lock(pending_mutex_);
      for (auto& kv : pending_) to_cancel.push_back(kv.second);
      // 排队中/在途请求统一回收；执行者稍后的 Finish 只会是空操作。
      pending_.clear();
      in_flight_ = 0;
      idle_cv_.notify_all();
    }
    for (auto& pending : to_cancel) {
      OnSettledFunc callback;
      if (pending->Cancel(&callback) && callback) callback(ErrorCode::kCancelled, "");
    }
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      io_stopping_ = true;
      queue_.clear();
    }
    queue_cv_.notify_all();

    // 不提前 close socket：在途请求受统一 deadline 约束，关停按阶段二契约
    // 等待其自然兑现（或超时），避免调用方被无限期挂起。
    const bool on_io = io_thread_.joinable() &&
                       io_thread_.get_id() == std::this_thread::get_id();
    if (!on_io && io_thread_.joinable()) io_thread_.join();
    if (!on_io) {
      std::unique_lock<std::mutex> lock(pending_mutex_);
      idle_cv_.wait(lock, [this] { return in_flight_ == 0; });
      // 关停契约：先等在途请求自然兑现，再关闭适配器并唤醒任何残留 I/O。
      if (requester) requester->Close();
    }
  }

  std::string node_name;
  std::string service_name;
  std::unique_ptr<RpcRequester> requester;
  std::shared_ptr<IRegistry> registry;

  // 同步与异步共用的有界队列 + 单一 I/O 执行者。
  std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  std::deque<QueuedCall> queue_;
  bool io_stopping_ = false;  // 受 queue_mutex_ 保护
  std::thread io_thread_;

  std::mutex pending_mutex_;
  std::map<std::uint64_t, std::shared_ptr<PendingCall>> pending_;
  std::atomic<std::uint64_t> next_id_{1};
  std::condition_variable idle_cv_;
  std::size_t in_flight_ = 0;  // 受 pending_mutex_ 保护
  std::atomic<bool> shutdown_requested_{false};
  std::atomic<bool> shutdown_done_{false};

  std::atomic<std::uint64_t> sent_count_{0};
};

}  // namespace

Result<std::shared_ptr<ClientHandle>> CreateClientCore(
    const std::string& node_name, const std::string& service_name,
    const std::shared_ptr<InjectionContext>& ctx) {
  auto injection = ResolveInjection(ctx);
  auto endpoints = injection->endpoints;
  auto registry = injection->registry;
  auto requester = endpoints->CreateRequester();
  if (!requester) {
    return {nullptr, ErrorCode::kTransport,
            "CreateClient: endpoint create(RpcRequester) failed"};
  }
  auto core = std::make_shared<ClientCore>(std::move(requester), registry);
  core->node_name = node_name;
  core->service_name = service_name;
  core->StartIo();
  return {core, ErrorCode::kOk, ""};
}

WireResponse ClientSendBytes(ClientHandle* handle, const std::string& bytes,
                             std::chrono::milliseconds timeout) {
  auto* core = static_cast<ClientCore*>(handle);
  if (!core) return {ErrorCode::kNotReady, ""};
  // 同步请求也走同一有界队列；不出队不兑现，不占用调用线程做 I/O。
  auto promise = std::make_shared<std::promise<WireResponse>>();
  auto future = promise->get_future();
  core->Enqueue(bytes, timeout,
                [promise](ErrorCode code, const std::string& payload) {
                  promise->set_value(WireResponse{
                      code, code == ErrorCode::kOk ? payload : std::string{}});
                });
  return future.get();
}

std::uint64_t ClientAsyncSendBytes(ClientHandle* handle,
                                   const std::string& bytes,
                                   std::chrono::milliseconds timeout,
                                   OnSettledFunc on_settled) {
  auto* core = static_cast<ClientCore*>(handle);
  if (!core) {
    if (on_settled) on_settled(ErrorCode::kNotReady, "");
    return 0;
  }
  // deadline 在入队这一刻确定，排队时间计入总预算。
  return core->Enqueue(bytes, timeout, std::move(on_settled));
}

bool ClientCancelRequestCore(ClientHandle* handle, std::uint64_t id) {
  auto* core = static_cast<ClientCore*>(handle);
  // Client 关停/重建后旧 core 的标志已置位，旧句柄不能再取消任何请求。
  if (!core || core->shutdown_requested_.load(std::memory_order_acquire)) {
    return false;
  }
  std::shared_ptr<PendingCall> pending;
  {
    std::lock_guard<std::mutex> lock(core->pending_mutex_);
    auto it = core->pending_.find(id);
    if (it == core->pending_.end()) return false;
    pending = it->second;
  }
  OnSettledFunc callback;
  if (!pending->Cancel(&callback)) return false;
  if (callback) callback(ErrorCode::kCancelled, "");
  return true;
}

void ClientShutdownCore(ClientHandle* handle) {
  auto* core = static_cast<ClientCore*>(handle);
  if (core) core->ShutdownInternal();
}

bool ClientServiceIsReady(const ClientHandle* handle) {
  auto* core = static_cast<const ClientCore*>(handle);
  if (!core || core->shutdown_requested_.load()) return false;
  return core->registry->Has(RegistryKind::kService, core->service_name);
}

bool ClientWaitForService(const ClientHandle* handle,
                          std::optional<std::chrono::milliseconds> timeout) {
  auto* core = static_cast<const ClientCore*>(handle);
  if (!core || core->shutdown_requested_.load()) return false;
  const bool infinite = !timeout.has_value();
  const auto deadline =
      infinite ? std::chrono::steady_clock::time_point::max()
               : std::chrono::steady_clock::now() + *timeout;
  constexpr auto poll = std::chrono::milliseconds(50);
  while (true) {
    if (core->registry->Has(RegistryKind::kService, core->service_name)) {
      return true;
    }
    if (!infinite && std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(poll);
  }
}

EndpointStats ClientStats(const ClientHandle* handle) {
  EndpointStats stats;
  auto* core = static_cast<const ClientCore*>(handle);
  if (!core) return stats;
  stats.sent = core->sent_count_.load(std::memory_order_relaxed);
  return stats;
}

}  // namespace detail
}  // namespace firefly
