#ifndef FIREFLY_SERVICE_CLIENT_BASE_H_
#define FIREFLY_SERVICE_CLIENT_BASE_H_

/// @file client_base.h
/// @brief 客户端的抽象基类，供节点按基类持有不同类型的 Client。

#include <string>

namespace firefly {

/**
 * @brief 客户端基类：向指定名称的服务发送请求并获取响应。
 *
 * 类型擦除所需的接口；失败语义与线程安全约束见 Client<Request, Response>。
 */
class ClientBase {
 public:
  /// @brief 构造客户端基类。
  /// @param[in] service_name 目标服务名。
  explicit ClientBase(const std::string& service_name)
      : service_name_(service_name) {}
  /// @brief 虚析构，保证经基类指针删除时正确调用派生类析构。
  virtual ~ClientBase() = default;

  /// @brief 初始化客户端。
  /// @return 成功返回 true。
  virtual bool Init() = 0;

  /// @brief 关闭客户端，取消全部在途请求。
  virtual void Shutdown() = 0;

  /// @brief 取目标服务名。
  const std::string& ServiceName() const { return service_name_; }

  /// @brief 目标服务当前是否可被发现。
  virtual bool ServiceIsReady() const = 0;

 protected:
  std::string service_name_;
};

}  // namespace firefly

#endif  // FIREFLY_SERVICE_CLIENT_BASE_H_