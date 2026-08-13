#ifndef FIREFLY_SERVICE_CLIENT_H_
#define FIREFLY_SERVICE_CLIENT_H_

#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <utility>

#include "firefly/detail/handles.h"
#include "firefly/service/client_base.h"

namespace firefly {

namespace detail {

// 不透明实现句柄（定义于 detail/handles.h）

// —— 以下工厂/操作函数为接口声明，实现待补充 ——
template <typename Request, typename Response>
std::shared_ptr<ClientHandle> CreateClient(const std::string& node_name,
                                           const std::string& service_name);

template <typename Request, typename Response>
void ClientShutdown(ClientHandle* handle);

template <typename Request, typename Response>
std::shared_ptr<Response> ClientSendRequest(ClientHandle* handle,
                                            const std::shared_ptr<Request>& request,
                                            const std::chrono::milliseconds& timeout);

template <typename Request, typename Response>
std::shared_future<std::shared_ptr<Response>> ClientAsyncSendRequest(
    ClientHandle* handle, const std::shared_ptr<Request>& request);

template <typename Request, typename Response>
std::shared_future<std::shared_ptr<Response>> ClientAsyncSendRequestWithCallback(
    ClientHandle* handle, const std::shared_ptr<Request>& request,
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
  using SharedResponse = std::shared_ptr<Response>;
  using SharedPromise = std::shared_ptr<std::promise<SharedResponse>>;
  using SharedFuture = std::shared_future<SharedResponse>;
  using CallbackType = std::function<void(SharedFuture)>;

  Client(const std::string& node_name, const std::string& service_name)
      : ClientBase(service_name), node_name_(node_name) {}
  ~Client() override = default;

  bool Init() override {
    if (init_) {
      return true;
    }
    impl_ = detail::CreateClient<Request, Response>(node_name_, service_name_);
    init_ = impl_ != nullptr;
    return init_;
  }

  void Shutdown() override {
    detail::ClientShutdown<Request, Response>(impl_.get());
    impl_.reset();
    init_ = false;
  }

  // ---------- 同步请求 ----------

  // 阻塞至收到响应或超时；超时返回 nullptr
  SharedResponse SendRequest(
      SharedRequest request,
      const std::chrono::milliseconds& timeout = std::chrono::milliseconds(5000)) {
    return detail::ClientSendRequest<Request, Response>(impl_.get(), request, timeout);
  }

  SharedResponse SendRequest(
      const Request& request,
      const std::chrono::milliseconds& timeout = std::chrono::milliseconds(5000)) {
    auto ptr = std::make_shared<Request>(request);
    return SendRequest(ptr, timeout);
  }

  // ---------- 异步请求 ----------

  SharedFuture AsyncSendRequest(SharedRequest request) {
    return detail::ClientAsyncSendRequest<Request, Response>(impl_.get(), request);
  }

  // 异步请求，收到响应后调用回调 cb
  SharedFuture AsyncSendRequest(SharedRequest request, CallbackType&& cb) {
    return detail::ClientAsyncSendRequestWithCallback<Request, Response>(
        impl_.get(), request, std::move(cb));
  }

  SharedFuture AsyncSendRequest(const Request& request) {
    auto ptr = std::make_shared<Request>(request);
    return AsyncSendRequest(ptr);
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
};

}  // namespace firefly

#include "firefly/detail/client_impl.h"

#endif  // FIREFLY_SERVICE_CLIENT_H_