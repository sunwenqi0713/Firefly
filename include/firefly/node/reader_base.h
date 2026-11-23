#ifndef FIREFLY_NODE_READER_BASE_H_
#define FIREFLY_NODE_READER_BASE_H_

#include <cstdint>
#include <string>

#include "firefly/common/types.h"

namespace firefly {

/**
 * @class ReaderBase
 * @brief 订阅者基类。Reader 订阅一个通道，可选用回调或 Observe 两种模式消费数据。
 */
class ReaderBase {
 public:
  ReaderBase() = default;
  virtual ~ReaderBase() = default;

  virtual bool Init() = 0;
  virtual void Shutdown() = 0;

  // 把新收到的消息拉取到“已观察”缓存（Observe 模式）
  virtual void Observe() = 0;
  // 清空已缓存数据
  virtual void ClearData() = 0;

  // 已观察缓存是否为空
  virtual bool Empty() const = 0;
  // 自上次清空后是否收到过数据
  virtual bool HasReceived() const = 0;
  // 距最近一次收到消息的时间间隔（秒），未收到过返回 -1
  virtual double GetDelaySec() const = 0;
  // 实时积压统计由 Stats().queue_depth 提供（阶段三：删除假 getter
  // PendingQueueSize，原实现返回配置值而非实际队列长度）。
  virtual EndpointStats Stats() const = 0;

  // 是否有 Writer 在发布本订阅通道
  virtual bool HasWriter() { return false; }

  virtual const std::string& GetChannelName() const = 0;
  virtual bool IsInit() const = 0;
};

}  // namespace firefly

#endif  // FIREFLY_NODE_READER_BASE_H_