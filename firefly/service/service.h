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
//
// 回调执行模型与并发契约（阶段二：契约基线，签名未改）：
//   - ServiceCallback 在 Service 专属的单个工作线程（worker_）内**内联派发**，
//     不是线程池；同一 Service 内请求严格串行（工作线程逐条处理），不同
//     Service 各有工作线程、彼此并行。
//   - 禁止在回调内调用本 Service 的 Shutdown()：Shutdown 会 join worker_，
//     而回调正运行在该线程上 → 自连接死锁。
//   - 回调抛出的异常不传播：工作线程捕获 std::exception（附 what()）或
//     catch(...)，统一映射为 ServiceResult{ErrorCode::kServiceFailed} 并
//     记录带 service 名的 ERROR 日志，响应帧携带该码回到客户端。
//   - 回调返回值约束：code==kOk 必须给非空 response，否则框架改判为
//     kServiceFailed（见 service_impl.h WorkerLoop）。
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

  // @post 调用后本 Service 不可再用；工作线程已 join、socket 已关闭、注册已注销。
  // 再次调用 Shutdown() 无副作用（ServiceImpl::Shutdown 早退）。Service 无对外
  // 业务方法，客户端在注册中心查不到后 ServiceIsReady/WaitForService 返回 false。
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
