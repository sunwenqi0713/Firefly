#ifndef FIREFLY_SERVICE_CLIENT_H_
#define FIREFLY_SERVICE_CLIENT_H_

/// @file client.h
/// @brief 请求方端点 Client<Request, Response>。
///
/// 失败语义与取消裁决写在各声明处；RPC 状态机、队列上界与时间边界的完整
/// 契约见 docs/architecture/contracts.md §5。

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
 * @brief 请求方端点：向指定名称的 Service 发起请求应答调用。
 *
 * 一个 Client 只能请求一个 Service。由 Node::CreateClient() 创建并返回
 * shared_ptr；Node 析构不销毁端点，须由持有者调用 Node::DeleteClient()
 * 或释放最后一个引用。
 *
 * @tparam Request  请求消息类型，须为 google::protobuf::Message 的派生类。
 * @tparam Response 响应消息类型，同上。
 *
 * @par 线程安全
 * SendRequest 与 AsyncSendRequest 共用同一个 I/O 执行者，可从多个线程并发
 * 调用；RequestHandle::Cancel() 亦可并发调用。不得与同一 Client 的
 * Shutdown() 或析构并发（内部句柄会被释放）。
 *
 * @sa docs/architecture/contracts.md §5 「RPC 请求状态与失败语义」
 */
template <typename Request, typename Response>
class Client : public ClientBase {
 public:
  using SharedRequest = std::shared_ptr<Request>;             ///< 可变请求指针。
  using ConstSharedRequest = std::shared_ptr<const Request>;  ///< 只读请求指针，发送接口的入参类型。
  using SharedResponse = std::shared_ptr<Response>;           ///< 响应指针。
  using SharedPromise = std::shared_ptr<std::promise<SharedResponse>>;  ///< 内部用的共享 promise。
  using SharedFuture = std::shared_future<SharedResponse>;    ///< 可复制的 future，句柄与回调共用。
  using CallbackType = std::function<void(SharedFuture&)>;    ///< 回调重载的签名，参数为已就绪的 future。
  using ResultType = ServiceResult<Response>;                 ///< 同步调用的返回类型。

  /**
   * @brief 可取消的异步请求句柄。
   *
   * future 保证在截止时间内兑现：成功时兑现 Response，失败时由 get() 抛出
   * FireflyException，不存在永不完成的情况。句柄可以复制和跨线程传递；
   * 它只弱引用内部实现，Client 析构后 Cancel() 返回 false 而不是访问悬空指针。
   */
  struct RequestHandle {
    std::uint64_t id = 0;   ///< 请求标识，仅在同一个 Client 内唯一。
    SharedFuture future;    ///< 结果通道；未成功提交时已带异常。

    /// @brief 请求取消。
    /// @return 已取消且 future 将以 kCancelled 兑现时返回 true；请求已完成、
    ///         已兑现、已被取消过、Client 已 Shutdown 或已重新 Init() 时返回 false。
    bool Cancel() { return cancel_fn_ ? cancel_fn_(id) : false; }

   private:
    friend class Client;
    std::function<bool(std::uint64_t)> cancel_fn_;
  };

  /**
   * @brief 构造客户端，此时尚未接入服务。
   *
   * @param[in] node_name    所属节点名，用于日志与注册。
   * @param[in] service_name 目标服务名；客户端按此名查询服务地址。
   * @param[in] ctx          测试注入点；传 nullptr 使用默认后端与默认注册中心。
   * @note 正常业务路径应使用 Node::CreateClient()，它负责 Init() 并把失败
   *       原因回填到 Result 中。
   */
  Client(const std::string& node_name, const std::string& service_name,
         const std::shared_ptr<detail::InjectionContext>& ctx = nullptr)
      : ClientBase(service_name), node_name_(node_name), ctx_(ctx) {}
  /// @brief 析构客户端；不隐式取消在途请求，需要确定语义时先调用 Shutdown()。
  ~Client() override = default;

  /// @brief 初始化客户端。
  /// @return 成功返回 true；失败返回 false，原因由 InitErrorCode() 给出。
  /// @note 不建立到服务端的连接，连接在首次请求时按服务发现结果建立。
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

  /// @brief Init() 失败时的错误码，供 Node::CreateClient() 回填 Result。
  /// @return 最近一次 Init() 的错误码；成功时为 kOk。
  ErrorCode InitErrorCode() const noexcept { return init_code_; }

  /// @brief Init() 失败时的定位线索。
  /// @return 最近一次 Init() 的错误信息；成功时为空串。
  const std::string& InitErrorMessage() const noexcept { return init_message_; }

  /**
   * @brief 关闭客户端。
   *
   * 依次停止接纳新请求、以 kCancelled 兑现全部在途请求、等待 I/O 执行者退出，
   * 最后释放内部句柄。
   *
   * @post 本 Client 不可再用；此前发出的 RequestHandle::Cancel() 恒返回 false。
   * @warning 不得在任意回调内调用，也不得与 SendRequest()/AsyncSendRequest() 并发。
   */
  void Shutdown() override {
    detail::ClientShutdownCore(impl_.get());
    impl_.reset();
    init_ = false;
  }

  // ---------- 同步请求 ----------

  /**
   * @brief 同步发起一次请求，阻塞至收到响应或超时。
   *
   * @param[in] request 请求消息。
   * @param[in] timeout 端到端预算，从入队时刻起算；排队、服务查询、连接、发送、
   *                    接收共用同一绝对截止时间，任一步耗尽即以 kTimeout 失败。
   * @return 调用结果；失败原因见 code，不会出现"所有失败都是空指针"的情形。
   * @retval kOk              response 非空，正常结果。
   * @retval kNotReady        客户端未初始化或已 Shutdown。
   * @retval kSerialization   请求序列化失败，或响应帧非法、反序列化失败。
   * @retval kServiceNotFound 服务发现中无该服务或没有可连接端点，请求未提交。
   * @retval kQueueFull       本地请求队列已满，请求未提交。
   * @retval kTimeout         已提交但未在截止时间前收到响应；对端是否执行未知。
   * @retval kServiceFailed   服务端业务失败，具体码由响应帧带回。
   * @retval kTransport       其他传输层失败。
   * @retval kCancelled       Client 被 Shutdown 时在途请求统一以此码结束。
   *
   * @note 有副作用的请求应自带幂等键或明确的重试策略：超时（kTimeout）只表示
   *       本地未等到响应，不代表服务端未执行。
   */
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

  /// @brief 同步发起一次请求，按值传入并在内部拷贝一次。
  /// @param[in] request 请求消息。
  /// @param[in] timeout 端到端预算，含义与返回码见接收 shared_ptr 的重载。
  ResultType SendRequest(
      const Request& request,
      const std::chrono::milliseconds& timeout = std::chrono::milliseconds(5000)) {
    return SendRequest(std::make_shared<Request>(request), timeout);
  }

  // ---------- 异步请求 ----------

  /**
   * @brief 异步发起一次请求，立即返回可取消的句柄。
   *
   * @param[in] request 请求消息。
   * @param[in] timeout 端到端预算，从入队时刻起算，语义与 SendRequest() 相同。
   * @return 请求句柄。即使提交失败也已携带一个已兑现异常的 future：句柄的
   *         id 为 0，`future.get()` 抛出 FireflyException。
   *
   * @exception FireflyException future.get() 在以下情形抛出，可用 code() 区分：
   *            kNotReady（客户端未初始化或已关停）、kSerialization（请求序列化
   *            失败或响应无法解析）、kQueueFull（本地请求队列已满）、kTimeout
   *            （截止时间前未收到响应）、kCancelled（被 Cancel() 或 Shutdown()
   *            撤销）、kServiceNotFound（服务不可发现）、kServiceFailed（服务端
   *            业务失败）、kTransport（其他传输层失败）。
   *
   * @note 每个请求只兑现一次；取消后到达的响应被丢弃。框架不做请求去重或
   *       幂等表，同一业务请求 Cancel 后重发由调用方负责去重。
   */
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
    // 句柄绑定内部实现的弱引用而非裸指针：Client 析构后只要请求仍在途，
    // Cancel() 依然安全；内部实现释放后返回 false。
    std::weak_ptr<detail::ClientHandle> weak_impl = impl_;
    handle.cancel_fn_ = [weak_impl](std::uint64_t rid) {
      auto impl = weak_impl.lock();
      if (!impl) return false;
      return detail::ClientCancelRequestCore(impl.get(), rid);
    };
    return handle;
  }

  /**
   * @brief 异步发起一次请求，并在 future 兑现后触发回调。
   *
   * @param[in] request 请求消息。
   * @param[in] cb      结果回调，参数是已就绪的 future；回调内调用 get() 可取得
   *                    响应或捕获异常。为空的回调等价于不传。
   * @param[in] timeout 端到端预算，含义与返回码见另一个 AsyncSendRequest() 重载。
   * @return 请求句柄，可用于取消。
   * @warning 回调在独立线程中执行，框架不管理其生命周期；回调必须自己保证
   *          所捕获对象的存活，且不得在回调内调用本 Client 的 Shutdown()。
   * @note 该重载为兼容旧调用方而保留，新代码建议直接使用返回的句柄。
   */
  RequestHandle AsyncSendRequest(
      ConstSharedRequest request, CallbackType cb,
      std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
    if (!cb) return AsyncSendRequest(request, timeout);
    RequestHandle handle = AsyncSendRequest(request, timeout);
    SharedFuture future = handle.future;
    std::thread([future, cb]() mutable {
      future.wait();
      cb(future);
    }).detach();
    return handle;
  }

  /// @brief 异步发起一次请求，按值传入并在内部拷贝一次。
  /// @param[in] request 请求消息。
  /// @param[in] timeout 端到端预算，含义与返回码见接收 shared_ptr 的重载。
  RequestHandle AsyncSendRequest(
      const Request& request,
      std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
    return AsyncSendRequest(std::make_shared<Request>(request), timeout);
  }

  // ---------- 服务可用性 / 统计 ----------

  /// @brief 目标服务当前是否可用。
  /// @return 发现记录中存在同名服务时返回 true。
  /// @note 只表示服务可被发现，不代表连接与调用一定成功。
  bool ServiceIsReady() const override {
    return detail::ClientServiceIsReady(impl_.get());
  }

  /**
   * @brief 等待目标服务变为可用。
   *
   * @param[in] timeout 等待上限；nullopt 表示无限等待，0ms 表示只检查一次当前状态。
   * @return 在期限内观察到服务时返回 true，超时返回 false。
   */
  bool WaitForService(
      std::optional<std::chrono::milliseconds> timeout = std::nullopt) {
    return detail::ClientWaitForService(impl_.get(), timeout);
  }

  /**
   * @brief 取累计统计快照。
   *
   * @return sent 为完成一次请求应答交换的条数；Client 不统计 dropped 与
   *         queue_depth，这两个字段恒为 0。
   * @par 线程安全
   * 计数由内部原子量维护，可从任意线程调用；不得与 Shutdown() 并发。
   */
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
