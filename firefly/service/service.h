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
// 回调执行模型与并发契约（阶段二：契约基线，阶段三保持）：
//   - ServiceCallback 在 Service 专属的单个工作线程（worker_）内**内联派发**，
//     不是线程池；同一 Service 内请求严格串行，不同 Service 彼此并行。
//   - 禁止在回调内调用本 Service 的 Shutdown()：Shutdown 会 join worker_，
//     而回调正运行在该线程上 → 自连接死锁。
//   - 回调抛出的异常不传播：工作线程捕获 std::exception（附 what()）或
//     catch(...)，统一映射为 ServiceResult{ErrorCode::kServiceFailed} 并
//     记录带 service 名的 ERROR 日志，响应帧携带该码回到客户端。
//   - 回调返回值约束：code==kOk 必须给非空 response，否则框架改判为
//     kServiceFailed（见 service_core.cc）。
//   - Service::Shutdown() 与全局 Clear() 都会等待在途回调返回。
//
// ---- 第 1 批：Service 生命周期契约 ----
//   显式 Shutdown() 与 ~Service() 复用同一条 detail::ServiceCore::Shutdown()
//   路径，幂等；"停止接收 → worker 退出 → 注册注销 → 关闭 socket"只执行一次。
//   worker 只持有 core 的 weak_ptr，所以释放最后一个外部 shared_ptr 必然触发
//   停机，旧客户端不再能调用该服务。
//   停机允许在回调线程内触发（回调里 Shutdown()，或释放最后一个引用导致
//   ~Service() 在该线程执行）：此时不 join 自身、不等待当前在途回调，只置
//   running_=false 并完成注销；worker 在本次回调返回后自行退出。若 core 的
//   析构正发生在 worker 线程上，则 detach 线程兜底，避免 terminate。
//   从其他线程调用 Shutdown()/析构则 join worker 并等待在途回调后才返回。
//
// 阶段三（任务 2）：本模板只做 protobuf 序列化 shim，nng 交互全部下沉到
// detail/service_core.cc；公共头不再包含 detail/*_impl.h 或 nng 头。
// ===========================================================================

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
          const ServiceCallback& service_callback,
          const std::shared_ptr<detail::InjectionContext>& ctx = nullptr)
      : ServiceBase(service_name),
        node_name_(node_name),
        service_callback_(service_callback),
        ctx_(ctx) {}
  // 释放最后一个外部引用即停服：显式 Shutdown 关闭 worker、注销服务，
  // 避免工作线程持有 core 导致"reset 后服务仍在运行"（审查 P1）。
  ~Service() override { Shutdown(); }

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

  // 供 Node::CreateService 回填 Result 的诊断信息
  ErrorCode InitErrorCode() const noexcept { return init_code_; }
  const std::string& InitErrorMessage() const noexcept { return init_message_; }

  // @post 工作线程已 join、socket 已关闭、在途回调已返回、注册已注销。
  void Shutdown() override {
    detail::ServiceShutdownCore(impl_.get());
    impl_.reset();
    init_ = false;
  }

  // @threadsafe 累计计数由 core 内原子量维护；queue_depth 对 Service 恒为 0。
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
