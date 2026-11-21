#include "firefly/detail/endpoint_api.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "firefly/common/log.h"
#include "firefly/detail/rpc_frame.h"

namespace firefly {
namespace detail {
namespace {

struct PendingCall {
  OnSettledFunc on_settled;
  std::atomic<bool> cancelled{false};
  std::mutex settle_mutex;
  bool settled = false;  // 保护：promise/回调只兑现一次
};

void Settle(PendingCall* pending, ErrorCode code, const std::string& payload) {
  if (!pending) return;
  OnSettledFunc callback;
  {
    std::lock_guard<std::mutex> lock(pending->settle_mutex);
    if (pending->settled) return;
    pending->settled = true;
    callback = std::move(pending->on_settled);
  }
  if (callback) callback(code, payload);
}

// 取消必须在同一把锁内完成"检查已兑现/已取消 + 标记 + 摘出回调"，
// 否则重复 Cancel 会误报 true（阶段二 RequestHandle 契约要求第二次返回 false）。
bool CancelPending(PendingCall* pending, OnSettledFunc* callback) {
  std::lock_guard<std::mutex> lock(pending->settle_mutex);
  if (pending->settled || pending->cancelled.load()) return false;
  pending->cancelled.store(true);
  pending->settled = true;
  *callback = std::move(pending->on_settled);
  return true;
}

struct ClientCore final : ClientHandle {
  ClientCore(std::shared_ptr<ITransportFactory> factory,
             std::shared_ptr<IRegistry> registry)
      : transport(std::move(factory)), registry(std::move(registry)) {}

  ~ClientCore() override {
    std::unique_lock<std::mutex> lock(pending_mutex_);
    idle_cv_.wait(lock, [this] { return in_flight_ == 0; });
    if (socket) socket->Close();
  }

  bool EnsureConnected() {
    if (socket && socket->Valid()) return true;
    auto urls = registry->List(RegistryKind::kService, service_name);
    if (urls.empty()) return false;
    socket = transport->Create(TransportKind::kReq);
    if (!socket) return false;
    if (socket->Dial(urls.front()) != 0) {
      FY_WARN("client[%s]: dial failed", service_name.c_str());
      socket->Close();
      socket.reset();
      return false;
    }
    return true;
  }

  using Deadline = std::chrono::steady_clock::time_point;

  static int RemainingMs(Deadline deadline) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return 0;
    return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                deadline - now).count());
  }

  // deadline 在请求进入客户端时计算，覆盖排队等待 io_mutex_ 的时间；
  // should_cancel 在拿到 io_mutex_ 后、真正 Send 前复查，杜绝"已取消仍发送"。
  WireResponse SendBytes(const std::string& bytes, Deadline deadline,
                         const std::function<bool()>& should_cancel) {
    std::lock_guard<std::mutex> lock(io_mutex_);  // REQ 单在途
    WireResponse out;
    // 排队已耗尽预算：尚未发送，按超时兑现（不触碰连接状态机）。
    if (RemainingMs(deadline) <= 0) {
      out.code = ErrorCode::kTimeout;
      return out;
    }
    // 取消可能发生在排队等待锁期间；拿锁后必须复查（审查 P1）。
    if (should_cancel && should_cancel()) {
      out.code = ErrorCode::kCancelled;
      return out;
    }
    if (!EnsureConnected()) {
      const bool registered =
          registry->Has(RegistryKind::kService, service_name);
      out.code = registered ? ErrorCode::kTransport : ErrorCode::kServiceNotFound;
      return out;
    }
    int send_ms = RemainingMs(deadline);
    if (send_ms <= 0) {
      out.code = ErrorCode::kTimeout;
      return out;
    }
    socket->SetSendTimeout(send_ms);
    int r = socket->Send(bytes);
    if (r != 0) {
      FY_WARN("client[%s]: send: %d", service_name.c_str(), r);
      out.code = ErrorCode::kTransport;
      return out;
    }
    sent_count_.fetch_add(1, std::memory_order_relaxed);

    // 接收预算 = 截止时间减去排队 + 发送已耗时间（不再各用一份完整预算）。
    int recv_ms = RemainingMs(deadline);
    if (recv_ms <= 0) {
      out.code = ErrorCode::kTimeout;
      socket->Close();
      socket.reset();
      return out;
    }
    socket->SetRecvTimeout(recv_ms);
    std::string frame;
    r = socket->Recv(&frame, recv_ms);
    if (r != kIoOk) {
      FY_WARN("client[%s]: recv: %d", service_name.c_str(), r);
      out.code = (r == kIoTimeout) ? ErrorCode::kTimeout : ErrorCode::kTransport;
      if (r == kIoTimeout) {
        // REQ 超时后状态机可能失配，关闭以重置（与阶段二一致）。
        socket->Close();
        socket.reset();
      }
      return out;
    }
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

  std::string node_name;
  std::string service_name;
  std::shared_ptr<ITransportFactory> transport;
  std::shared_ptr<IRegistry> registry;
  std::unique_ptr<ISocket> socket;
  std::mutex io_mutex_;

  std::mutex pending_mutex_;
  std::map<std::uint64_t, std::shared_ptr<PendingCall>> pending_;
  std::atomic<std::uint64_t> next_id_{1};
  std::condition_variable idle_cv_;
  std::size_t in_flight_ = 0;  // 受 pending_mutex_ 保护
  std::atomic<bool> shutdown_requested_{false};

  std::atomic<std::uint64_t> sent_count_{0};
};

}  // namespace

Result<std::shared_ptr<ClientHandle>> CreateClientCore(
    const std::string& node_name, const std::string& service_name,
    const std::shared_ptr<InjectionContext>& ctx) {
  auto injection = ctx ? ctx : DefaultInjection();
  auto transport = injection->transport ? injection->transport
                                        : DefaultTransportFactory();
  auto registry =
      injection->registry ? injection->registry : DefaultRegistry();
  auto core = std::make_shared<ClientCore>(transport, registry);
  core->node_name = node_name;
  core->service_name = service_name;
  return {core, ErrorCode::kOk, ""};
}

WireResponse ClientSendBytes(ClientHandle* handle, const std::string& bytes,
                             std::chrono::milliseconds timeout) {
  auto* core = static_cast<ClientCore*>(handle);
  if (!core) return {ErrorCode::kNotReady, ""};
  // deadline 在请求进入客户端时确定，排队时间计入总预算。
  const auto deadline =
      std::chrono::steady_clock::now() + timeout;
  return core->SendBytes(bytes, deadline, nullptr);
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
  auto pending = std::make_shared<PendingCall>();
  pending->on_settled = std::move(on_settled);
  if (core->shutdown_requested_.load()) {
    Settle(pending.get(), ErrorCode::kNotReady, "");
    return 0;
  }
  const std::uint64_t id = core->next_id_.fetch_add(1);
  {
    std::lock_guard<std::mutex> lock(core->pending_mutex_);
    core->pending_[id] = pending;
    ++core->in_flight_;
  }

  // deadline 在此刻（请求提交时）确定；should_cancel 在拿到 io_mutex_ 后复查。
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  const std::string service = core->service_name;
  std::thread([core, bytes, deadline, id, pending, service]() {
    WireResponse response;
    if (pending->cancelled.load()) {
      response.code = ErrorCode::kCancelled;
    } else {
      response = core->SendBytes(bytes, deadline,
                                 [pending]() { return pending->cancelled.load(); });
      if (pending->cancelled.load()) {
        FY_WARN("client[%s]: late response for cancelled request %llu dropped",
                service.c_str(), static_cast<unsigned long long>(id));
        response.code = ErrorCode::kCancelled;
        response.payload.clear();
      }
    }
    Settle(pending.get(), response.code, response.payload);

    std::lock_guard<std::mutex> lock(core->pending_mutex_);
    core->pending_.erase(id);
    if (--core->in_flight_ == 0) core->idle_cv_.notify_all();
  }).detach();
  return id;
}

bool ClientCancelRequestCore(ClientHandle* handle, std::uint64_t id) {
  auto* core = static_cast<ClientCore*>(handle);
  if (!core) return false;
  std::shared_ptr<PendingCall> pending;
  {
    std::lock_guard<std::mutex> lock(core->pending_mutex_);
    auto it = core->pending_.find(id);
    if (it == core->pending_.end()) return false;
    pending = it->second;
  }
  OnSettledFunc callback;
  if (!CancelPending(pending.get(), &callback)) return false;
  if (callback) callback(ErrorCode::kCancelled, "");
  return true;
}

void ClientShutdownCore(ClientHandle* handle) {
  auto* core = static_cast<ClientCore*>(handle);
  if (!core) return;
  core->shutdown_requested_.store(true);

  std::vector<std::shared_ptr<PendingCall>> to_cancel;
  {
    std::lock_guard<std::mutex> lock(core->pending_mutex_);
    for (auto& kv : core->pending_) to_cancel.push_back(kv.second);
  }
  for (auto& pending : to_cancel) {
    pending->cancelled.store(true);
    Settle(pending.get(), ErrorCode::kCancelled, "");
  }
  {
    std::unique_lock<std::mutex> lock(core->pending_mutex_);
    core->idle_cv_.wait(lock, [core] { return core->in_flight_ == 0; });
  }
  if (core->socket) {
    core->socket->Close();
    core->socket.reset();
  }
}

bool ClientServiceIsReady(const ClientHandle* handle) {
  auto* core = static_cast<const ClientCore*>(handle);
  if (!core) return false;
  return core->registry->Has(RegistryKind::kService, core->service_name);
}

bool ClientWaitForService(const ClientHandle* handle,
                          std::optional<std::chrono::milliseconds> timeout) {
  auto* core = static_cast<const ClientCore*>(handle);
  if (!core) return false;
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
