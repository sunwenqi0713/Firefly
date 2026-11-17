#ifndef FIREFLY_DETAIL_CLIENT_IMPL_H_
#define FIREFLY_DETAIL_CLIENT_IMPL_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nng/nng.h>
#include <nng/protocol/reqrep0/req.h>

#include "firefly/common/log.h"
#include "firefly/detail/handles.h"
#include "firefly/detail/rpc_frame.h"
#include "firefly/discovery.h"
#include "firefly/message/message_traits.h"

namespace firefly {
namespace detail {

// REQ 客户端实现：REQ socket 与一个 REP 服务端点连接。
// 由于 REQ socket 同一时刻仅支持一个在途请求，所有操作经内部互斥串行化。
// 阶段二：新增 pending 请求表与在途计数，支撑 Cancel 与 Shutdown 等待。
template <typename Request, typename Response>
struct ClientImpl : public ClientHandle {
  using SharedResponse = std::shared_ptr<Response>;
  using SharedPromise = std::shared_ptr<std::promise<SharedResponse>>;

  std::string node_name;
  std::string service_name;

  nng_socket sock_{0};
  mutable std::mutex io_mutex_;

  // ---- 阶段二：异步请求生命周期状态 ----
  struct PendingReq {
    SharedPromise promise;
    std::atomic<bool> cancelled{false};
    std::mutex settle_mutex;
    bool settled = false;  // 受 settle_mutex 保护，保证 promise 只兑现一次
  };
  std::mutex pending_mutex_;
  std::map<std::uint64_t, std::shared_ptr<PendingReq>> pending_;
  std::atomic<std::uint64_t> next_id_{1};
  std::condition_variable idle_cv_;
  std::size_t in_flight_ = 0;   // 受 pending_mutex_ 保护
  std::atomic<bool> shutdown_requested_{false};

  ~ClientImpl() override {
    // 析构兜底：正常路径由 ClientShutdown 完成等待；此处仅防止遗漏时泄漏线程。
    while (true) {
      std::unique_lock<std::mutex> lk(pending_mutex_);
      if (in_flight_ == 0) break;
      idle_cv_.wait(lk);
    }
    if (sock_.id != 0) {
      nng_close(sock_);
      sock_.id = 0;
    }
  }

  // 确保与注册中心中该服务的某个端点建立连接
  bool EnsureConnected() {
    if (sock_.id != 0) return true;
    auto urls = discovery::Registry::instance().List(
        discovery::Kind::kService, service_name);
    if (urls.empty()) return false;
    int r = nng_req0_open(&sock_);
    if (r != 0) {
      FY_ERROR("client[%s]: nng_req0_open: %s", service_name.c_str(),
               nng_strerror(r));
      return false;
    }
    nng_dialer d{NNG_DIALER_INITIALIZER};
    r = nng_dial(sock_, urls.front().c_str(), &d, 0);
    if (r != 0) {
      FY_WARN("client[%s]: dial %s: %s", service_name.c_str(),
              urls.front().c_str(), nng_strerror(r));
      nng_close(sock_);
      sock_.id = 0;
      return false;
    }
    return true;
  }
};

// ---------- 接口层声明对应的操作函数 ----------

template <typename Request, typename Response>
Result<std::shared_ptr<ClientHandle>> CreateClient(const std::string& node_name,
                                                   const std::string& service_name) {
  static_assert(message::HasSerializer<Request>::value &&
                    message::HasSerializer<Response>::value,
                "Request/Response must derive from protobuf::Message.");

  auto impl = std::make_shared<ClientImpl<Request, Response>>();
  impl->node_name = node_name;
  impl->service_name = service_name;
  return {impl, ErrorCode::kOk, ""};
}

// 兑现 promise 的辅助：只有第一个调用者生效（settled 保护）。
// code != kOk 时以 FireflyException(code, msg) 兑现；成功时给 response。
template <typename Request, typename Response>
void SettlePending(typename ClientImpl<Request, Response>::PendingReq* req,
                   ErrorCode code, const std::string& service,
                   std::shared_ptr<Response> response) {
  if (req == nullptr) return;
  std::lock_guard<std::mutex> lk(req->settle_mutex);
  if (req->settled) return;
  req->settled = true;
  if (code == ErrorCode::kOk) {
    req->promise->set_value(std::move(response));
  } else {
    const std::string msg = "AsyncSendRequest[service='" + service + "']: " +
                            ErrorCodeToString(code);
    req->promise->set_exception(
        std::make_exception_ptr(FireflyException(code, msg)));
  }
}

template <typename Request, typename Response>
ServiceResult<Response> ClientSendRequest(
    ClientHandle* handle, const std::shared_ptr<const Request>& request,
    const std::chrono::milliseconds& timeout) {
  using R = ServiceResult<Response>;
  auto* impl = static_cast<ClientImpl<Request, Response>*>(handle);
  if (!impl || !request) {
    return {ErrorCode::kSerialization, nullptr};
  }
  std::lock_guard<std::mutex> lk(impl->io_mutex_);

  if (!impl->EnsureConnected()) {
    const bool registered = discovery::Registry::instance().Has(
        discovery::Kind::kService, impl->service_name);
    FY_WARN("client[%s]: no service endpoint", impl->service_name.c_str());
    return {registered ? ErrorCode::kTransport : ErrorCode::kServiceNotFound,
            nullptr};
  }

  std::string bytes;
  if (!message::MessageTraits<Request>::Serialize(*request, &bytes)) {
    return {ErrorCode::kSerialization, nullptr};
  }
  nng_setopt_ms(impl->sock_, NNG_OPT_SENDTIMEO,
                static_cast<int>(timeout.count()));
  nng_setopt_ms(impl->sock_, NNG_OPT_RECVTIMEO,
                static_cast<int>(timeout.count()));

  int r = nng_send(impl->sock_, const_cast<char*>(bytes.data()), bytes.size(), 0);
  if (r != 0) {
    FY_WARN("client[%s]: send: %s", impl->service_name.c_str(),
            nng_strerror(r));
    return {ErrorCode::kTransport, nullptr};
  }
  void* data = nullptr;
  size_t size = 0;
  r = nng_recv(impl->sock_, &data, &size, NNG_FLAG_ALLOC);
  if (r != 0) {
    FY_WARN("client[%s]: recv: %s", impl->service_name.c_str(),
            nng_strerror(r));
    const ErrorCode ec =
        (r == NNG_ETIMEDOUT) ? ErrorCode::kTimeout : ErrorCode::kTransport;
    if (r == NNG_ETIMEDOUT) {
      // REQ 协议超时后 socket 状态可能不一致，关闭以重置
      nng_close(impl->sock_);
      impl->sock_.id = 0;
    }
    return {ec, nullptr};
  }
  std::string frame(static_cast<char*>(data), size);
  nng_free(data, size);

  ErrorCode frame_code = ErrorCode::kSerialization;
  std::string payload;
  if (!DecodeResponse(frame, &frame_code, &payload)) {
    FY_WARN("client[%s]: malformed response frame (%zu bytes)",
            impl->service_name.c_str(), frame.size());
    return {ErrorCode::kSerialization, nullptr};
  }
  if (frame_code != ErrorCode::kOk) {
    return {frame_code, nullptr};
  }

  auto resp = std::make_shared<Response>();
  if (!message::MessageTraits<Response>::Deserialize(payload, resp.get())) {
    return {ErrorCode::kSerialization, nullptr};
  }
  return {ErrorCode::kOk, resp};
}

// 异步接口：detach 线程调同步 SendRequest，结果写入 pending 表的 promise。
// 失败以 FireflyException 在 future 中兑现；成功返回 shared_ptr<Response>。
// 阶段二：新增 timeout 参数（不再硬编码），返回 request id 供 Cancel 使用。
template <typename Request, typename Response>
std::uint64_t ClientAsyncSendRequest(
    ClientHandle* handle, const std::shared_ptr<const Request>& request,
    std::chrono::milliseconds timeout, std::shared_future<std::shared_ptr<Response>>* out_future) {
  using SharedResponse = std::shared_ptr<Response>;
  auto* impl = static_cast<ClientImpl<Request, Response>*>(handle);
  auto promise = std::make_shared<std::promise<SharedResponse>>();
  std::shared_future<SharedResponse> future = promise->get_future();
  if (out_future) *out_future = future;

  if (impl == nullptr) {
    promise->set_exception(std::make_exception_ptr(
        FireflyException(ErrorCode::kNotReady, "AsyncSendRequest: null client")));
    return 0;
  }
  if (impl->shutdown_requested_.load()) {
    promise->set_exception(std::make_exception_ptr(FireflyException(
        ErrorCode::kNotReady, "AsyncSendRequest: client is shutting down")));
    return 0;
  }

  std::uint64_t id = impl->next_id_.fetch_add(1);
  auto pending = std::make_shared<typename ClientImpl<Request, Response>::PendingReq>();
  pending->promise = promise;
  {
    std::lock_guard<std::mutex> lk(impl->pending_mutex_);
    impl->pending_[id] = pending;
    ++impl->in_flight_;
  }

  // 拷贝 service 名供日志使用，避免线程持有已释放的 impl 字段
  const std::string service = impl->service_name;
  std::thread([handle, request, timeout, id, pending, service]() mutable {
    // 若在被调度前已取消，直接兑现取消码，不发请求
    if (pending->cancelled.load()) {
      SettlePending<Request, Response>(pending.get(), ErrorCode::kCancelled,
                                       service, nullptr);
    } else {
      auto r = ClientSendRequest<Request, Response>(handle, request, timeout);
      if (pending->cancelled.load()) {
        // 迟到响应：不覆盖已兑现的 kCancelled，直接丢弃（不做请求去重表）
        FY_WARN("client[%s]: late response for cancelled request %llu dropped",
                service.c_str(), static_cast<unsigned long long>(id));
        SettlePending<Request, Response>(pending.get(), ErrorCode::kCancelled,
                                         service, nullptr);
      } else {
        SettlePending<Request, Response>(pending.get(), r.code, service,
                                         r.response);
      }
    }

    auto* pimpl = static_cast<ClientImpl<Request, Response>*>(handle);
    if (pimpl) {
      std::lock_guard<std::mutex> lk(pimpl->pending_mutex_);
      pimpl->pending_.erase(id);
      --pimpl->in_flight_;
      if (pimpl->in_flight_ == 0) pimpl->idle_cv_.notify_all();
    }
  }).detach();

  return id;
}

// 带回调版本：回调在 promise 兑现后于 detach 线程内调用（回调内 get 可拿结果/异常）。
template <typename Request, typename Response>
std::uint64_t ClientAsyncSendRequestWithCallback(
    ClientHandle* handle, const std::shared_ptr<const Request>& request,
    std::chrono::milliseconds timeout,
    std::function<void(std::shared_future<std::shared_ptr<Response>>&)> cb,
    std::shared_future<std::shared_ptr<Response>>* out_future) {
  auto* impl = static_cast<ClientImpl<Request, Response>*>(handle);
  // 复用无回调版本拿到 id/future，再挂一层观察回调无法挂到内部线程，
  // 因此这里单独实现：额外起一个等待 future 的小线程调 cb。
  const std::uint64_t id =
      ClientAsyncSendRequest<Request, Response>(handle, request, timeout, out_future);
  if (id == 0 || !cb) return id;
  std::shared_future<std::shared_ptr<Response>> fut = *out_future;
  std::thread([fut, cb]() mutable { cb(fut); }).detach();
  (void)impl;
  return id;
}

// 取消指定在途请求：返回 false 的情形见 client.h RequestHandle::Cancel 注释。
template <typename Request, typename Response>
bool ClientCancelRequest(ClientHandle* handle, std::uint64_t id) {
  auto* impl = static_cast<ClientImpl<Request, Response>*>(handle);
  if (impl == nullptr) return false;
  std::shared_ptr<typename ClientImpl<Request, Response>::PendingReq> pending;
  {
    std::lock_guard<std::mutex> lk(impl->pending_mutex_);
    auto it = impl->pending_.find(id);
    if (it == impl->pending_.end()) return false;  // 已完成 / 已取消移除
    pending = it->second;
  }
  pending->cancelled.store(true);
  SettlePending<Request, Response>(pending.get(), ErrorCode::kCancelled,
                                   impl->service_name, nullptr);
  return true;
}

// 关停：置位 → 取消全部在途 → 等待在途归零 → 关闭 socket。
// 释放后到达的响应帧：socket 已关闭，发送方收到错误；客户端不再处理。
template <typename Request, typename Response>
void ClientShutdown(ClientHandle* handle) {
  auto* impl = static_cast<ClientImpl<Request, Response>*>(handle);
  if (!impl) return;
  impl->shutdown_requested_.store(true);

  std::vector<std::shared_ptr<typename ClientImpl<Request, Response>::PendingReq>> to_cancel;
  {
    std::lock_guard<std::mutex> lk(impl->pending_mutex_);
    for (auto& kv : impl->pending_) to_cancel.push_back(kv.second);
  }
  for (auto& p : to_cancel) {
    p->cancelled.store(true);
    SettlePending<Request, Response>(p.get(), ErrorCode::kCancelled,
                                     impl->service_name, nullptr);
  }

  {
    std::unique_lock<std::mutex> lk(impl->pending_mutex_);
    impl->idle_cv_.wait(lk, [impl] { return impl->in_flight_ == 0; });
  }
  if (impl->sock_.id != 0) {
    nng_close(impl->sock_);
    impl->sock_.id = 0;
  }
}

template <typename Request, typename Response>
bool ClientServiceIsReady(const ClientHandle* handle) {
  auto* impl = static_cast<const ClientImpl<Request, Response>*>(handle);
  if (!impl) return false;
  return discovery::Registry::instance().Has(
      discovery::Kind::kService, impl->service_name);
}

template <typename Request, typename Response>
bool ClientWaitForService(const ClientHandle* handle,
                          std::chrono::nanoseconds timeout) {
  auto* impl = static_cast<const ClientImpl<Request, Response>*>(handle);
  if (!impl) return false;

  // timeout < 0 表示无限等待；timeout == 0 仅检查当前状态。
  const auto infinite = timeout.count() < 0;
  const auto deadline =
      infinite ? std::chrono::steady_clock::time_point::max()
               : std::chrono::steady_clock::now() + timeout;

  constexpr auto poll = std::chrono::milliseconds(50);
  while (true) {
    if (discovery::Registry::instance().Has(discovery::Kind::kService,
                                            impl->service_name)) {
      return true;
    }
    if (!infinite && std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(poll);
  }
}

}  // namespace detail
}  // namespace firefly

#endif  // FIREFLY_DETAIL_CLIENT_IMPL_H_
