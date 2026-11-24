#ifndef FIREFLY_NODE_READER_BASE_H_
#define FIREFLY_NODE_READER_BASE_H_

/// @file reader_base.h
/// @brief 订阅者的抽象基类，供节点按基类持有不同类型的 Reader。

#include <cstdint>
#include <string>

#include "firefly/common/types.h"

namespace firefly {

/**
 * @brief 订阅者基类：订阅一个通道，以回调或 Observe 两种模式消费数据。
 *
 * 实现细节与线程安全约束见 Reader<MessageT>；本类只描述类型擦除所需的接口。
 */
class ReaderBase {
 public:
  /// @brief 构造基类；派生类负责初始化。
  ReaderBase() = default;

  /// @brief 虚析构，保证经基类指针删除时正确调用派生类析构。
  virtual ~ReaderBase() = default;

  /// @brief 接入通道并开始接收。
  /// @return 成功返回 true。
  virtual bool Init() = 0;

  /// @brief 关闭端点，停止接收并等待在途回调返回。
  virtual void Shutdown() = 0;

  /// @brief 触发一次拉取。
  /// @note 本实现中消息在收线程直接入队，该调用不产生额外效果。
  virtual void Observe() = 0;

  /// @brief 清空观察缓存。
  virtual void ClearData() = 0;

  /// @brief 观察缓存是否为空。
  virtual bool Empty() const = 0;

  /// @brief 自上次清空后是否收到过消息。
  virtual bool HasReceived() const = 0;

  /// @brief 距最近一次收到消息的时间间隔。
  /// @return 秒数；从未收到过返回 -1。
  virtual double GetDelaySec() const = 0;

  /// @brief 取累计统计快照；实时积压由其中的 queue_depth 提供。
  virtual EndpointStats Stats() const = 0;

  /// @brief 通道当前是否有 Writer 在发布。
  virtual bool HasWriter() { return false; }

  /// @brief 取订阅的通道名。
  virtual const std::string& GetChannelName() const = 0;

  /// @brief 是否已 Init() 且未 Shutdown()。
  virtual bool IsInit() const = 0;
};

}  // namespace firefly

#endif  // FIREFLY_NODE_READER_BASE_H_