#ifndef FIREFLY_SERVICE_SERVICE_H_
#define FIREFLY_SERVICE_SERVICE_H_

#include <functional>
#include <memory>
#include <string>

#include "firefly/detail/handles.h"
#include "firefly/service/service_base.h"

namespace firefly {

namespace detail {

// 不透明实现句柄（定义于 detail/handles.h）

// —— 以下工厂/操作函数为接口声明，实现待补充 ——
template <typename Request, typename Response>
std::shared_ptr<ServiceHandle> CreateService(
    const std::string& node_name, const std::string& service_name,
    const std::function<void(const std::shared_ptr<Request>&,
                             std::shared_ptr<Response>&)>& callback);

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
  // 服务回调：处理请求并填充响应
  using ServiceCallback =
      std::function<void(const std::shared_ptr<Request>&,
                         std::shared_ptr<Response>&)>;

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
    impl_ = detail::CreateService<Request, Response>(node_name_, service_name_,
                                                     service_callback_);
    init_ = impl_ != nullptr;
    return init_;
  }

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
};

}  // namespace firefly

#include "firefly/detail/service_impl.h"

#endif  // FIREFLY_SERVICE_SERVICE_H_