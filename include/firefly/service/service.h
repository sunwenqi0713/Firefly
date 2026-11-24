#ifndef FIREFLY_SERVICE_SERVICE_H_
#define FIREFLY_SERVICE_SERVICE_H_

/// @file service.h
/// @brief 应答方端点 Service<Request, Response>。
///
/// 回调返回值的错误语义与关停等待写在各声明处；错误码到响应帧的映射见
/// docs/architecture/contracts.md §8。

#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "firefly/common/log.h"
#include "firefly/common/types.h"
#include "firefly/detail/endpoint_api.h"
#include "firefly/message/message_traits.h"
#include "firefly/service/service_base.h"

namespace firefly {

/**
 * @brief 服务：接收 Client 的请求，调用业务回调产出响应并返回。
 *
 * 由 Node::CreateService() 创建并返回 shared_ptr。释放最后一个引用即停服，
 * Node 析构不销毁端点，须由持有者调用 Node::DeleteService() 或释放引用。
 *
 * @tparam Request  请求消息类型，须为 google::protobuf::Message 的派生类。
 * @tparam Response 响应消息类型，同上。
 *
 * @par 回调执行
 * 回调在本 Service 专属的单个工作线程内内联派发，不在创建它的线程上执行：
 * 同一 Service 的请求严格串行，不同 Service 之间并行。回调抛出的异常不会
 * 传播，而是记录一条带服务名的 ERROR 日志并映射为 kServiceFailed 回给客户端。
 *
 * @sa docs/architecture/contracts.md §3 「并发与执行位置」
 */
template <typename Request, typename Response>
class Service : public ServiceBase {
 public:
  /// @brief 服务回调的返回类型；与 Client 共用，保证错误码端到端可传递。
  using ServiceResult = firefly::ServiceResult<Response>;

  /**
   * @brief 服务回调的签名：处理一个请求并返回带错误码的响应。
   *
   * 返回 kOk 时必须携带非空 response，否则框架改判为 kServiceFailed；
   * 返回 kServiceFailed 表示业务处理失败，response 可为空。业务失败的详细
   * 原因无法随错误码传递，需要时请把原因编码进 response 消息。
   */
  using ServiceCallback =
      std::function<ServiceResult(const std::shared_ptr<const Request>&)>;

  /**
   * @brief 构造服务，此时尚未开始受理请求。
   *
   * @param[in] node_name        所属节点名，用于注册与日志。
   * @param[in] service_name     服务名，供客户端按名查找；须全局唯一才不会被
   *                             客户端在多个同名端点间任意选择。
   * @param[in] service_callback 业务回调；不得为空。
   * @param[in] ctx              测试注入点；传 nullptr 使用默认后端与默认注册中心。
   * @note 正常业务路径应使用 Node::CreateService()，它负责 Init() 并把失败
   *       原因回填到 Result 中。
   */
  Service(const std::string& node_name, const std::string& service_name,
          const ServiceCallback& service_callback,
          const std::shared_ptr<detail::InjectionContext>& ctx = nullptr)
      : ServiceBase(service_name),
        node_name_(node_name),
        service_callback_(service_callback),
        ctx_(ctx) {}
  /**
   * @brief 析构即停服：工作线程退出、注册注销、在途回调返回。
   * @note 显式 Shutdown() 与本析构复用同一条停机路径且幂等，不会重复执行。
   */
  ~Service() override { Shutdown(); }

  /// @brief 开始受理请求。
  /// @return 成功返回 true；失败返回 false，原因由 InitErrorCode() 给出。
  /// @post 成功后 IsInit() 为 true，工作线程已启动且服务已可被客户端发现。
  bool Init() override {
    if (init_) {
      return true;
    }
    detail::OnRequestFunc on_request =
        [callback = service_callback_](const std::string& req_bytes,
                                       ErrorCode* code) -> std::string {
      auto request = std::make_shared<Request>();
      if (!message::MessageTraits<Request>::Deserialize(req_bytes,
                                                        request.get())) {
        *code = ErrorCode::kSerialization;
        return "";
      }
      ServiceResult result;
      try {
        result = callback(request);
      } catch (const std::exception& e) {
        // 与 service_core.cc 的兜底一致：what() 进日志，业务失败码回客户端。
        FY_ERROR("service: callback threw: %s", e.what());
        *code = ErrorCode::kServiceFailed;
        return "";
      } catch (...) {
        FY_ERROR("service: callback threw (non-std exception)");
        *code = ErrorCode::kServiceFailed;
        return "";
      }
      if (result.code == ErrorCode::kOk && !result.response) {
        result.code = ErrorCode::kServiceFailed;
      }
      std::string payload;
      if (result.code == ErrorCode::kOk &&
          !message::MessageTraits<Response>::Serialize(*result.response,
                                                       &payload)) {
        *code = ErrorCode::kSerialization;
        return "";
      }
      *code = result.code;
      return payload;
    };

    auto r = detail::CreateServiceCore(node_name_, service_name_, on_request,
                                       ctx_);
    init_code_ = r.code;
    init_message_ = std::move(r.message);
    if (!r) {
      return false;
    }
    impl_ = std::move(r.value);
    init_ = true;
    return true;
  }

  /// @brief Init() 失败时的错误码，供 Node::CreateService() 回填 Result。
  /// @return 最近一次 Init() 的错误码；成功时为 kOk。
  ErrorCode InitErrorCode() const noexcept { return init_code_; }

  /// @brief Init() 失败时的定位线索。
  /// @return 最近一次 Init() 的错误信息；成功时为空串。
  const std::string& InitErrorMessage() const noexcept { return init_message_; }

  /**
   * @brief 关闭服务：停止受理、注销注册、等待在途回调返回。
   *
   * @post 工作线程已退出、注册已注销、在途回调已返回；本 Service 不可再用。
   * @note 在回调内调用不会死锁（框架跳过自连接与自等待），但会立即停止受理
   *       新请求；需要确定的停机完成语义时应由外部线程调用。
   */
  void Shutdown() override {
    detail::ServiceShutdownCore(impl_.get());
    impl_.reset();
    init_ = false;
  }

  /**
   * @brief 取累计统计快照。
   *
   * @return recv 为收到并派发到回调的请求条数，sent 为成功写回的响应条数；
   *         dropped 与 queue_depth 对 Service 恒为 0。
   * @par 线程安全
   * 计数由内部原子量维护，可从任意线程调用；不得与 Shutdown() 并发。
   */
  EndpointStats Stats() const { return detail::ServiceStats(impl_.get()); }

 private:
  std::string node_name_;
  ServiceCallback service_callback_;
  std::shared_ptr<detail::InjectionContext> ctx_;
  std::shared_ptr<detail::ServiceHandle> impl_;
  bool init_ = false;
  ErrorCode init_code_ = ErrorCode::kOk;
  std::string init_message_;
};

}  // namespace firefly

#endif  // FIREFLY_SERVICE_SERVICE_H_
