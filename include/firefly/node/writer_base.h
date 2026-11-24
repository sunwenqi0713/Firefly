#ifndef FIREFLY_NODE_WRITER_BASE_H_
#define FIREFLY_NODE_WRITER_BASE_H_

/// @file writer_base.h
/// @brief 发布者的抽象基类，供节点按基类持有不同类型的 Writer。

#include <string>

namespace firefly {

/**
 * @brief 发布者基类：向指定通道发送消息。
 *
 * 类型擦除所需的接口；送达语义与线程安全约束见 Writer<MessageT>。
 *
 * @warning 一个 Writer 只能写一个通道；多个 Writer 可以写同一通道。
 */
class WriterBase {
 public:
  /// @brief 构造基类；派生类负责初始化。
  WriterBase() = default;

  /// @brief 虚析构，保证经基类指针删除时正确调用派生类析构。
  virtual ~WriterBase() = default;

  /// @brief 接入通道并完成注册。
  /// @return 成功返回 true。
  virtual bool Init() = 0;

  /// @brief 关闭端点，之后的发送调用失败。
  virtual void Shutdown() = 0;

  /// @brief 通道当前是否有 Reader 订阅。
  virtual bool HasReader() { return false; }

  /// @brief 取写入的通道名。
  virtual const std::string& GetChannelName() const = 0;

  /// @brief 是否已 Init() 且未 Shutdown()。
  virtual bool IsInit() const = 0;
};

}  // namespace firefly

#endif  // FIREFLY_NODE_WRITER_BASE_H_