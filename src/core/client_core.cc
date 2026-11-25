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
//   kQueued --Begin--> kStarting --CommitSend--> kSending --> kDone（Settle）
//   kQueued / kStarting --Cancel--> kDone（未提交，服务端 0 次）
//   kSending --Cancel--> kDone（已提交，只结束本地等待，远端可能执行）
// 因此"取消能否阻止发送"只有一次裁决：CommitSend 是唯一提交点。提交之前的
// 取消一定不会触碰传输；提交之后本地仍可提前结束等待，但远端是否执行未知。
struct PendingCall {
  enum class Stage { kQueued, kStarting, kSending, kDone };

  explicit PendingCall(OnSettledFunc cb) : on_settled(std::move(cb)) {}

  // 执行者出队后取得处理权；返回 false 表示排队期间已被取消。
  bool Begin() {
    std::lock_guard<std::mutex> lock(state_mutex);
    if (stage != Stage::kQueued) return false;
    stage = Stage::kStarting;
    return true;
  }

  // 提交点：预算检查与服务发现已通过、即将调用后端 Exchange。返回 false
  // 表示取消在此前获胜，调用方不得发送。
  bool CommitSend() {
    std::lock_guard<std::mutex> lock(state_mutex);
    if (stage != Stage::kStarting) return false;
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
    if (expiry_thread_.joinable()) expiry_thread_.detach();
  }

  void StartIo() {
    io_thread_ = std::thread([this] { IoLoop(); });
    expiry_thread_ = std::thread([this] { ExpiryLoop(); });
  }

  static int RemainingMs(Deadline deadline) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return 0;
    return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                deadline - now).count());
  }

  // 阶段一：预算检查与服务发现。不触碰传输，因此这一阶段获胜的取消必然
  // 让服务端执行次数为 0。发现查询本身不接收 deadline（见 known-gaps G3），
  // 但返回后立刻重算预算：一旦过期就不再提交发送。
  ErrorCode ResolveTarget(const QueuedCall& call, std::string* address) {
    if (shutdown_requested_.load(std::memory_order_acquire)) {
      return ErrorCode::kNotReady;
    }
    if (RemainingMs(call.deadline) <= 0) return ErrorCode::kTimeout;
    auto records = registry->Query(RegistryKind::kService, service_name);
    if (shutdown_requested_.load(std::memory_order_acquire)) {
      return ErrorCode::kNotReady;
    }
    if (RemainingMs(call.deadline) <= 0) return ErrorCode::kTimeout;
    if (records.empty()) return ErrorCode::kServiceNotFound;
    *address = records.front().address;
    return ErrorCode::kOk;
  }

  // 阶段二：已过提交点。连接/重连/发送/接收以及各步剩余预算由适配器在
  // 同一绝对 deadline 上完成；Core 只映射返回值与解帧。
  WireResponse ExchangeCommitted(const std::string& address,
                                 const std::string& bytes, Deadline deadline) {
    WireResponse out;
    std::string frame;
    TransferStatus status =
        requester->Exchange(address, bytes, deadline, &frame);
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

  // 一次请求的完整路径：发现 → 提交点 → 传输。返回 kCancelled 表示提交前
  // 已被取消（此时服务端未收到任何数据）。
  WireResponse RunCall(const QueuedCall& call) {
    WireResponse out{ErrorCode::kCancelled, ""};
    std::string address;
    const ErrorCode resolved = ResolveTarget(call, &address);
    if (resolved != ErrorCode::kOk) {
      out.code = resolved;
      return out;
    }
    if (!call.pending->CommitSend()) return out;  // 取消已获胜，不发送
    return ExchangeCommitted(address, call.bytes, call.deadline);
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
    // notify_all：到期线程与执行者都等在这条条件变量上。
    queue_cv_.notify_all();
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
      // 排队期间已被取消（Begin 失败）时整条路径都不执行：服务端 0 次。
      if (call.pending->Begin()) out = RunCall(call);
      Finish(call.id, call.pending.get(), out);
    }
  }

  // 到期线程：只负责"排队中"的请求，与执行者是否被前一条请求占住无关。
  // 这样排队请求在截止时间点兑现 kTimeout，而不是等执行者腾出手来才发现
  // 已经过期。在途请求（已提交）由适配器在同一绝对 deadline 上结束。
  void ExpiryLoop() {
    for (;;) {
      std::vector<QueuedCall> expired;
      {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        for (;;) {
          if (io_stopping_) return;
          if (queue_.empty()) {
            queue_cv_.wait(lock);
            continue;
          }
          const auto earliest = EarliestDeadlineLocked();
          const auto now = std::chrono::steady_clock::now();
          if (now < earliest) {
            queue_cv_.wait_until(lock, earliest);
            continue;
          }
          std::deque<QueuedCall> kept;
          for (auto& call : queue_) {
            if (call.deadline <= now) {
              expired.push_back(std::move(call));
            } else {
              kept.push_back(std::move(call));
            }
          }
          queue_.swap(kept);
          break;
        }
      }
      // 兑现与计数回收都在 queue_mutex_ 之外完成，回调可在其中调用 Cancel。
      for (auto& call : expired) ExpireQueued(call);
    }
  }

  // 调用前持有 queue_mutex_。
  Deadline EarliestDeadlineLocked() const {
    Deadline earliest = Deadline::max();
    for (const auto& call : queue_) {
      if (call.deadline < earliest) earliest = call.deadline;
    }
    return earliest;
  }

  // 与 Finish 相同：先退出 pending 表、递减计数，再兑现，避免 in_flight_ 泄漏。
  void ExpireQueued(QueuedCall& call) {
    {
      std::lock_guard<std::mutex> lock(pending_mutex_);
      if (pending_.erase(call.id) != 0 && in_flight_ > 0 && --in_flight_ == 0) {
        idle_cv_.notify_all();
      }
    }
    call.pending->Settle(ErrorCode::kTimeout, "");
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

    // 关闭适配器，让仍在 Exchange 里的请求立即以 kClosed 返回，而不是等它
    // 自然到期。兑现权在上一段已由 Cancel 取走（统一兑现 kCancelled），这里
    // 的返回值只会被 Finish 丢弃，因此不影响"每请求只兑现一次"。
    if (requester) requester->Close();

    const bool on_io = io_thread_.joinable() &&
                       io_thread_.get_id() == std::this_thread::get_id();
    if (expiry_thread_.joinable() &&
        expiry_thread_.get_id() != std::this_thread::get_id()) {
      expiry_thread_.join();
    }
    if (!on_io && io_thread_.joinable()) io_thread_.join();
    if (!on_io) {
      std::unique_lock<std::mutex> lock(pending_mutex_);
      idle_cv_.wait(lock, [this] { return in_flight_ == 0; });
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
  std::thread expiry_thread_;

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
