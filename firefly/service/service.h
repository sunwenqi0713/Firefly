#ifndef FIREFLY_SERVICE_SERVICE_H_
#define FIREFLY_SERVICE_SERVICE_H_

// ===========================================================================
// 服务端的错误语义（显式契约，阶段一）
// ===========================================================================
//
// ServiceCallback 返回 ServiceResult<Response>，而不是 void + 输出引用：
//   - code == kOk：response 非空，正常返回；
//   - code == kServiceFailed：业务处理失败（如参数非法、除零），response 可为空；
//   - code == kSerialization：请求反序列化失败，框架直接返回该码，不调用业务回调。
//
// code 通过响应帧的 status 字段跨进程传到客户端（见 detail/rpc_frame.h），
// 客户端的 SendRequest 可据此区分 kServiceFailed / kTimeout /
// kServiceNotFound / kSerialization，不再全部折叠为 nullptr。
//
// 已知约束（本阶段如实记录，不在范围内修复）：
//   - 业务失败的详细原因无法自动传递（ServiceResult 无 message 字段）；
//     如需细节，业务方应把错误信息编码进 response。
//   - 回调执行线程与并发模型见后续阶段契约；当前回调在服务端工作线程执行。
// ===========================================================================

#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "firefly/common/types.h"
#include "firefly/detail/handles.h"
#include "firefly/service/service_base.h"

namespace firefly {

namespace detail {

// 不透明实现句柄（定义于 detail/handles.h）

template <typename Request, typename Response>
Result<std::shared_ptr<ServiceHandle>> CreateService(
    const std::string& node_name, const std::string& service_name,
    const std::function<ServiceResult<Response>(
        const std::shared_ptr<const Request>&)>& callback);

template <typename Request, typename Response>
void ServiceShutdown(ServiceHandle* handle);

}  // namespace detail

/**
 * @brief 服务：接收 Client 的 Request，调用用户回调产出 Response 并返回。
 *
 * @tparam Request  请求类型
 * @tparam Response 响应类型
 */
template <typename Request, typename Response>
class Service : public ServiceBase {
 public:
  using ServiceResult = firefly::ServiceResult<Response>;

  // 服务回调：处理请求并返回带错误码的结果
  using ServiceCallback =
      std::function<ServiceResult(const std::shared_ptr<const Request>&)>;

  Service(const std::string& node_name, const std::string& service_name,
          const ServiceCallback& service_callback)
      : ServiceBase(service_name),
        node_name_(node_name),
        service_callback_(service_callback) {}
  ~Service() override = default;

  bool Init() override {
    if (init_) {
      return true;
    }
    auto r = detail::CreateService<Request, Response>(node_name_, service_name_,
                                                      service_callback_);
    init_code_ = r.code;
    init_message_ = std::move(r.message);
    if (!r) {
      return false;
    }
    impl_ = std::move(r.value);
    init_ = true;
    return true;
  }

  // 供 Node::CreateService 回填 Result 的诊断信息
  ErrorCode InitErrorCode() const noexcept { return init_code_; }
  const std::string& InitErrorMessage() const noexcept { return init_message_; }

  void Shutdown() override {
    detail::ServiceShutdown<Request, Response>(impl_.get());
    impl_.reset();
    init_ = false;
  }

 private:
  std::string node_name_;
  ServiceCallback service_callback_;
  std::shared_ptr<detail::ServiceHandle> impl_;
  bool init_ = false;
  ErrorCode init_code_ = ErrorCode::kOk;
  std::string init_message_;
};

}  // namespace firefly

#include "firefly/detail/service_impl.h"

#endif  // FIREFLY_SERVICE_SERVICE_H_
