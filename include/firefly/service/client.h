#ifndef FIREFLY_SERVICE_CLIENT_H_
#define FIREFLY_SERVICE_CLIENT_H_

// ===========================================================================
// 客户端的错误语义与生命周期契约（阶段一错误语义；阶段二取消/关停；阶段三去泄漏）
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
// ---- 阶段二：取消与关停契约（阶段三保持不变）----
// AsyncSendRequest 返回 RequestHandle{id, future, Cancel()}：
//   - timeout 参数显式化（默认 5000ms）；超时后 future 以
//     FireflyException{kTimeout} 兑现，不再出现"永不兑现的 future"。
//   - Cancel() 返回 true 表示已取消并使 future 以 kCancelled 兑现；
//     返回 false 的情形：请求已完成/已兑现、已被取消过、Client 已 Shutdown。
//   - Cancel 后服务端响应迟到：直接丢弃。不做请求去重/幂等表 —— 同一请求
//     Cancel 后再 Send 不保证幂等，由上层业务负责。
// Client::Shutdown() 语义：置位关停 → 取消全部在途请求（以 kCancelled 兑现）
//   → 停止 I/O 执行者并等待在途请求退出 → 关闭 socket 释放 impl_。
//
// ---- 第 1 批：同步/异步共用截止时间与两态取消 ----
// SendRequest 与 AsyncSendRequest 的 timeout 是端到端预算（从入队这一刻起
// 算）：排队、服务查询、连接、发送、接收共用同一绝对 deadline，任一步耗尽
// 都以 kTimeout 失败；有界请求队列满时同步返回 kQueueFull、异步 future 抛
// FireflyException{kQueueFull}，不扩公共 API。
// Cancel 区分两态：排队中取消使服务端执行 0 次；已提交发送后取消只结束
// 本地等待（兑现 kCancelled），服务端仍可能执行该请求。每请求只兑现一次。
// Shutdown 后或同一 Client 重新 Init() 后，旧 RequestHandle::Cancel() 恒 false。
// @threadsafe SendRequest/AsyncSendRequest 共用 core 的单一 I/O 执行者；
//   Cancel 经 pending_mutex_ 保护，可并发调用；
//   不得与同一 Client 的 Shutdown()/析构并发（impl_ 生命周期竞态）。
//
// ---- 阶段三：可用性等待与统计 ----
// WaitForService(std::optional<milliseconds> timeout = std::nullopt)：
//   nullopt = 无限等待；0ms = 只检查当前状态。阶段二的负数魔法值已删除。
// Stats() 提供累计 sent 与 dropped（@threadsafe）；速率统计未实现（phase4）。
// 公共头不再包含 detail/client_impl.h 或 nng 头。
// ===========================================================================

#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "firefly/common/types.h"
#include "firefly/detail/endpoint_api.h"
#include "firefly/message/message_traits.h"
#include "firefly/service/client_base.h"

namespace firefly {

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
    // false=请求已完成/已兑现/已取消/Client 已 Shutdown 或已重新 Init。
    bool Cancel() { return cancel_fn_ ? cancel_fn_(id) : false; }

   private:
    friend class Client;
    std::function<bool(std::uint64_t)> cancel_fn_;
  };

  Client(const std::string& node_name, const std::string& service_name,
         const std::shared_ptr<detail::InjectionContext>& ctx = nullptr)
      : ClientBase(service_name), node_name_(node_name), ctx_(ctx) {}
  ~Client() override = default;

  bool Init() override {
    if (init_) {
      return true;
    }
    auto r = detail::CreateClientCore(node_name_, service_name_, ctx_);
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
  void Shutdown() override {
    detail::ClientShutdownCore(impl_.get());
    impl_.reset();
    init_ = false;
  }

  // ---------- 同步请求 ----------

  // 阻塞至收到响应或超时；失败时 result.code 指明原因，不再返回 nullptr
  ResultType SendRequest(
      ConstSharedRequest request,
      const std::chrono::milliseconds& timeout = std::chrono::milliseconds(5000)) {
    if (!init_ || !impl_ || !request) {
      return {ErrorCode::kNotReady, nullptr};
    }
    std::string bytes;
    if (!message::MessageTraits<Request>::Serialize(*request, &bytes)) {
      return {ErrorCode::kSerialization, nullptr};
    }
    auto wire = detail::ClientSendBytes(impl_.get(), bytes, timeout);
    if (wire.code != ErrorCode::kOk) {
      return {wire.code, nullptr};
    }
    auto response = std::make_shared<Response>();
    if (!message::MessageTraits<Response>::Deserialize(wire.payload,
                                                       response.get())) {
      return {ErrorCode::kSerialization, nullptr};
    }
    return {ErrorCode::kOk, response};
  }

  ResultType SendRequest(
      const Request& request,
      const std::chrono::milliseconds& timeout = std::chrono::milliseconds(5000)) {
    return SendRequest(std::make_shared<Request>(request), timeout);
  }

  // ---------- 异步请求 ----------

  // future.get() 成功返回 response；失败抛 FireflyException(ErrorCode, message)
  RequestHandle AsyncSendRequest(
      ConstSharedRequest request,
      std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
    RequestHandle handle;
    if (!init_ || !impl_ || !request) {
      std::promise<SharedResponse> failed;
      handle.future = failed.get_future().share();
      failed.set_exception(std::make_exception_ptr(FireflyException(
          ErrorCode::kNotReady, "AsyncSendRequest: client not ready")));
      return handle;
    }
    std::string bytes;
    if (!message::MessageTraits<Request>::Serialize(*request, &bytes)) {
      std::promise<SharedResponse> failed;
      handle.future = failed.get_future().share();
      failed.set_exception(std::make_exception_ptr(FireflyException(
          ErrorCode::kSerialization, "AsyncSendRequest: serialize failed")));
      return handle;
    }
    auto promise = std::make_shared<std::promise<SharedResponse>>();
    handle.future = promise->get_future().share();
    const std::string service = service_name_;
    auto on_settled = [promise, service](ErrorCode code,
                                         const std::string& payload) {
      if (code == ErrorCode::kOk) {
        auto response = std::make_shared<Response>();
        if (message::MessageTraits<Response>::Deserialize(payload,
                                                          response.get())) {
          promise->set_value(response);
          return;
        }
        code = ErrorCode::kSerialization;
      }
      promise->set_exception(std::make_exception_ptr(FireflyException(
          code, "AsyncSendRequest[service='" + service + "']: " +
                    std::string(ErrorCodeToString(code)))));
    };
    handle.id =
        detail::ClientAsyncSendBytes(impl_.get(), bytes, timeout, on_settled);
    // 句柄绑定 core 的弱引用而非裸 this：Client 析构后只要请求仍在途
    // （core 被后台线程持有），Cancel 依然安全；core 释放后返回 false。
    // 修复"保留句柄、释放客户端、再 Cancel 访问悬空指针"（审查 P1）。
    std::weak_ptr<detail::ClientHandle> weak_impl = impl_;
    handle.cancel_fn_ = [weak_impl](std::uint64_t rid) {
      auto impl = weak_impl.lock();
      if (!impl) return false;
      return detail::ClientCancelRequestCore(impl.get(), rid);
    };
    return handle;
  }

  // 兼容旧调用方的回调重载：回调在后台线程、future 兑现后触发。
  RequestHandle AsyncSendRequest(
      ConstSharedRequest request, CallbackType cb,
      std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
    if (!cb) return AsyncSendRequest(request, timeout);
    // 先取到 future，再挂一个等待线程调 cb；cb 内 get() 可拿结果/异常。
    RequestHandle handle = AsyncSendRequest(request, timeout);
    SharedFuture future = handle.future;
    std::thread([future, cb]() mutable {
      future.wait();
      cb(future);
    }).detach();
    return handle;
  }

  RequestHandle AsyncSendRequest(
      const Request& request,
      std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
    return AsyncSendRequest(std::make_shared<Request>(request), timeout);
  }

  // ---------- 服务可用性 / 统计 ----------

  bool ServiceIsReady() const override {
    return detail::ClientServiceIsReady(impl_.get());
  }

  // nullopt = 无限等待；0ms = 立即检查。阶段二负数魔法值已删除。
  bool WaitForService(
      std::optional<std::chrono::milliseconds> timeout = std::nullopt) {
    return detail::ClientWaitForService(impl_.get(), timeout);
  }

  // @threadsafe 累计 sent/dropped 由 core 内原子量维护；速率统计未实现。
  EndpointStats Stats() const { return detail::ClientStats(impl_.get()); }

 private:
  std::string node_name_;
  std::shared_ptr<detail::InjectionContext> ctx_;
  std::shared_ptr<detail::ClientHandle> impl_;
  bool init_ = false;
  ErrorCode init_code_ = ErrorCode::kOk;
  std::string init_message_;
};

}  // namespace firefly

#endif  // FIREFLY_SERVICE_CLIENT_H_
