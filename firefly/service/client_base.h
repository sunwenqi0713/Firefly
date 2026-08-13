#ifndef FIREFLY_SERVICE_CLIENT_BASE_H_
#define FIREFLY_SERVICE_CLIENT_BASE_H_

#include <string>

namespace firefly {

/**
 * @class ClientBase
 * @brief 客户端基类。客户端向指定名称的服务发送 Request 并获取 Response。
 */
class ClientBase {
 public:
  explicit ClientBase(const std::string& service_name)
      : service_name_(service_name) {}
  virtual ~ClientBase() = default;

  virtual bool Init() = 0;
  virtual void Shutdown() = 0;

  const std::string& ServiceName() const { return service_name_; }

  // 服务是否已就绪（是否存在同名且可用的 Service）
  virtual bool ServiceIsReady() const = 0;

 protected:
  std::string service_name_;
};

}  // namespace firefly

#endif  // FIREFLY_SERVICE_CLIENT_BASE_H_