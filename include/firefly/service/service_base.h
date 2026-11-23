#ifndef FIREFLY_SERVICE_SERVICE_BASE_H_
#define FIREFLY_SERVICE_SERVICE_BASE_H_

#include <string>

namespace firefly {

/**
 * @class ServiceBase
 * @brief 服务基类。服务接收 Client 的 Request 并返回 Response。
 */
class ServiceBase {
 public:
  explicit ServiceBase(const std::string& service_name)
      : service_name_(service_name) {}
  virtual ~ServiceBase() = default;

  virtual bool Init() = 0;
  virtual void Shutdown() = 0;

  const std::string& service_name() const { return service_name_; }

 protected:
  std::string service_name_;
};

}  // namespace firefly

#endif  // FIREFLY_SERVICE_SERVICE_BASE_H_