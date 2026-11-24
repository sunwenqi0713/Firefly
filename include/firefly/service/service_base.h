#ifndef FIREFLY_SERVICE_SERVICE_BASE_H_
#define FIREFLY_SERVICE_SERVICE_BASE_H_

/// @file service_base.h
/// @brief 服务的抽象基类，供节点按基类持有不同类型的 Service。

#include <string>

namespace firefly {

/**
 * @brief 服务基类：接收客户端请求并返回响应。
 *
 * 类型擦除所需的接口；回调语义与线程安全约束见
 * Service<Request, Response>。
 */
class ServiceBase {
 public:
  /// @brief 构造服务基类。
  /// @param[in] service_name 服务名。
  explicit ServiceBase(const std::string& service_name)
      : service_name_(service_name) {}
  /// @brief 虚析构，保证经基类指针删除时正确调用派生类析构。
  virtual ~ServiceBase() = default;

  /// @brief 开始受理请求。
  /// @return 成功返回 true。
  virtual bool Init() = 0;

  /// @brief 停止受理，注销注册并等待在途回调返回。
  virtual void Shutdown() = 0;

  /// @brief 取服务名。
  const std::string& service_name() const { return service_name_; }

 protected:
  std::string service_name_;
};

}  // namespace firefly

#endif  // FIREFLY_SERVICE_SERVICE_BASE_H_