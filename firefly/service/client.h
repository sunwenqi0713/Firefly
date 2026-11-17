#ifndef FIREFLY_SERVICE_CLIENT_H_
#define FIREFLY_SERVICE_CLIENT_H_

// ===========================================================================
// 客户端的错误语义与生命周期契约（阶段一错误语义；阶段二取消/关停）
// ===========================================================================
//
// SendRequest 返回 ServiceResult<Response>，失败原因按 code 区分：
//   kOk               response 非空，正常结果；
//   kServiceNotFound  注册中心中不存在该服务（或无可连接端点）；
//   kTimeout          已发出请求但等待响应超时（默认 5000ms）；
//   kServiceFailed    服务端业务失败（由响应帧 status 字段带回）；
//   kSerialization    请求序列化失败 / 响应帧非法 / 响应反序列化失败；
//   kTransport        其他 nng 层错误。
// 不再出现"所有失败都是 nullptr"的情形。
//
// AsyncSendRequest 的 future 在失败时抛 FireflyException(ErrorCode, message)，
// 成功时返回 shared_ptr<Response>；同步接口不用异常。
//
// ---- 阶段二：取消与关停契约 ----
// AsyncSendRequest 返回 RequestHandle{id, future, Cancel()}：
//   - timeout 参数显式化（默认 5000ms）；超时后 future 以
//     FireflyException{kTimeout} 兑现，不再出现"永不兑现的 future"。
//   - Cancel() 返回 true 表示已取消并使 future 以 kCancelled 兑现；
//     返回 false 的情形：请求已完成/已兑现、已被取消过、Client 已 Shutdown。
//   - Cancel 后服务端响应迟到：直接丢弃（pending 表项在取消时兑现 kCancelled，
//     迟到响应到达时不再覆盖）。不做请求去重/幂等表 —— 同一请求 Cancel 后
//     再 Send 不保证幂等，由上层业务负责。
// Client::Shutdown() 语义：置位关停 → 取消全部在途请求（以 kCancelled 兑现）
//   → 阻塞至全部在途请求兑现或取消 → 关闭 socket 释放 impl_。释放后到达的
//   响应帧因 socket 已关闭被 nng 丢弃，客户端不再处理。
// @threadsafe SendRequest 经 impl 内 io_mutex_ 串行化（REQ 单在途限制）；
//   AsyncSendRequest 与 Cancel 经 pending_mutex_ 保护，可并发调用；
//   不得与同一 Client 的 Shutdown()/析构并发（impl_ 生命周期竞态）。
// ===========================================================================

#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <utility>

#include "firefly/common/types.h"
#include "firefly/detail/handles.h"
#include "firefly/service/client_base.h"

namespace firefly {

namespace detail {

// 不透明实现句柄（定义于 detail/handles.h）

template <typename Request, typename Response>
Result<std::shared_ptr<ClientHandle>> CreateClient(const std::string& node_name,
                                                   const std::string& service_name);

template <typename Request, typename Response>
void ClientShutdown(ClientHandle* handle);

template <typename Request, typename Response>
ServiceResult<Response> ClientSendRequest(
    ClientHandle* handle, const std::shared_ptr<const Request>& request,
    const std::chrono::milliseconds& timeout);

template <typename Request, typename Response>
std::uint64_t ClientAsyncSendRequest(
    ClientHandle* handle, const std::shared_ptr<const Request>& request,
    std::chrono::milliseconds timeout,
    std::shared_future<std::shared_ptr<Response>>* out_future);

template <typename Request, typename Response>
std::uint64_t ClientAsyncSendRequestWithCallback(
    ClientHandle* handle, const std::shared_ptr<const Request>& request,
    std::chrono::milliseconds timeout,
    std::function<void(std::shared_future<std::shared_ptr<Response>>&)> cb,
    std::shared_future<std::shared_ptr<Response>>* out_future);

template <typename Request, typename Response>
bool ClientCancelRequest(ClientHandle* handle, std::uint64_t id);

template <typename Request, typename Response>
bool ClientServiceIsReady(const ClientHandle* handle);

template <typename Request, typename Response>
bool ClientWaitForService(const ClientHandle* handle,
                          std::chrono::nanoseconds timeout);

}  // namespace detail

/**
 * @brief 客户端：向指定服务发送 Request，并获取 Response。
 *
 * @warning 一个 Client 只能请求一个 Service。
 */
template <typename Request, typename Response>
class Client : public ClientBase {
 public:
  using SharedRequest = std::shared_ptr<Request>;
  using ConstSharedRequest = std::shared_ptr<const Request>;
  using SharedResponse = std::shared_ptr<Response>;
  using SharedPromise = std::shared_ptr<std::promise<SharedResponse>>;
  using SharedFuture = std::shared_future<SharedResponse>;
  using CallbackType = std::function<void(SharedFuture&)>;
  using ResultType = ServiceResult<Response>;

  // 可取消的异步请求句柄（阶段二）。
  struct RequestHandle {
    std::uint64_t id = 0;
    SharedFuture future;

    // 返回 true=已取消且 future 已以 kCancelled 兑现；
    // false=请求已完成/已兑现/已取消/Client 已 Shutdown。
    bool Cancel() { return cancel_fn_ ? cancel_fn_(id) : false; }

   private:
    friend class Client;
    std::function<bool(std::uint64_t)> cancel_fn_;
  };

  Client(const std::string& node_name, const std::string& service_name)
      : ClientBase(service_name), node_name_(node_name) {}
  ~Client() override = default;

  bool Init() override {
    if (init_) {
      return true;
    }
    auto r = detail::CreateClient<Request, Response>(node_name_, service_name_);
    init_code_ = r.code;
    init_message_ = std::move(r.message);
    if (!r) {
      return false;
    }
    impl_ = std::move(r.value);
    init_ = true;
    return true;
  }

  // 供 Node::CreateClient 回填 Result 的诊断信息
  ErrorCode InitErrorCode() const noexcept { return init_code_; }
  const std::string& InitErrorMessage() const noexcept { return init_message_; }

  // @post 调用后不可再用；阻塞至全部在途请求兑现/取消后释放 impl_。
  // 释放后到达的响应帧被丢弃（见文件头契约）。
  void Shutdown() override {
    detail::ClientShutdown<Request, Response>(impl_.get());
    impl_.reset();
    init_ = false;
  }

  // ---------- 同步请求 ----------

  // 阻塞至收到响应或超时；失败时 result.code 指明原因，不再返回 nullptr
  ResultType SendRequest(
      ConstSharedRequest request,
      const std::chrono::milliseconds& timeout = std::chrono::milliseconds(5000)) {
    if (!init_ || !impl_) {
      return {ErrorCode::kNotReady, nullptr};
    }
    return detail::ClientSendRequest<Request, Response>(impl_.get(), request,
                                                        timeout);
  }

  ResultType SendRequest(
      const Request& request,
      const std::chrono::milliseconds& timeout = std::chrono::milliseconds(5000)) {
    return SendRequest(std::make_shared<Request>(request), timeout);
  }

  // ---------- 异步请求 ----------

  // future.get() 成功返回 response；失败抛 FireflyException(ErrorCode, message)
  // timeout 显式化：超时以 FireflyException{kTimeout} 兑现。
  RequestHandle AsyncSendRequest(
      ConstSharedRequest request,
      std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
    RequestHandle h;
    std::uint64_t id = detail::ClientAsyncSendRequest<Request, Response>(
        impl_.get(), request, timeout, &h.future);
    h.id = id;
    h.cancel_fn_ = [this](std::uint64_t rid) {
      if (!impl_) return false;
      return detail::ClientCancelRequest<Request, Response>(impl_.get(), rid);
    };
    return h;
  }

  // 兼容旧调用方的回调重载：回调在后台线程、future 兑现后触发。
  RequestHandle AsyncSendRequest(
      ConstSharedRequest request, CallbackType cb,
      std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
    RequestHandle h;
    std::uint64_t id = detail::ClientAsyncSendRequestWithCallback<Request, Response>(
        impl_.get(), request, timeout, std::move(cb), &h.future);
    h.id = id;
    h.cancel_fn_ = [this](std::uint64_t rid) {
      if (!impl_) return false;
      return detail::ClientCancelRequest<Request, Response>(impl_.get(), rid);
    };
    return h;
  }

  RequestHandle AsyncSendRequest(
      const Request& request,
      std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
    return AsyncSendRequest(std::make_shared<Request>(request), timeout);
  }

  // ---------- 服务可用性 ----------

  bool ServiceIsReady() const override {
    return detail::ClientServiceIsReady<Request, Response>(impl_.get());
  }

  // 等待与 Service 建立连接；timeout 为 -1（默认）表示无限等待
  template <typename RatioT = std::milli>
  bool WaitForService(std::chrono::duration<int64_t, RatioT> timeout =
                          std::chrono::duration<int64_t, RatioT>(-1)) {
    return detail::ClientWaitForService<Request, Response>(
        impl_.get(), std::chrono::duration_cast<std::chrono::nanoseconds>(timeout));
  }

 private:
  std::string node_name_;
  std::shared_ptr<detail::ClientHandle> impl_;
  bool init_ = false;
  ErrorCode init_code_ = ErrorCode::kOk;
  std::string init_message_;
};

}  // namespace firefly

#include "firefly/detail/client_impl.h"

#endif  // FIREFLY_SERVICE_CLIENT_H_
