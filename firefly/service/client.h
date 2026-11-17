#ifndef FIREFLY_SERVICE_CLIENT_H_
#define FIREFLY_SERVICE_CLIENT_H_

// ===========================================================================
// 客户端的错误语义（显式契约，阶段一）
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
// 已知缺陷（本阶段 out-of-scope，不修）:
//   - AsyncSendRequest 无 timeout 参数，内部硬编码 5000ms；
//   - 无 cancel 机制；Shutdown 与在途异步请求的交互未定义（见审查问题 2）；
//   - 异步工作在 detach 线程，Client 过早析构可能与在途请求竞态。
// 这些将在"生命周期与取消"阶段处理。
// ===========================================================================

#include <chrono>
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
std::shared_future<std::shared_ptr<Response>> ClientAsyncSendRequest(
    ClientHandle* handle, const std::shared_ptr<const Request>& request);

template <typename Request, typename Response>
std::shared_future<std::shared_ptr<Response>> ClientAsyncSendRequestWithCallback(
    ClientHandle* handle, const std::shared_ptr<const Request>& request,
    std::function<void(std::shared_future<std::shared_ptr<Response>>&)> cb);

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
  using CallbackType = std::function<void(SharedFuture)>;
  using ResultType = ServiceResult<Response>;

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
  SharedFuture AsyncSendRequest(ConstSharedRequest request) {
    return detail::ClientAsyncSendRequest<Request, Response>(impl_.get(), request);
  }

  SharedFuture AsyncSendRequest(ConstSharedRequest request, CallbackType&& cb) {
    return detail::ClientAsyncSendRequestWithCallback<Request, Response>(
        impl_.get(), request, std::move(cb));
  }

  SharedFuture AsyncSendRequest(const Request& request) {
    return AsyncSendRequest(std::make_shared<Request>(request));
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
