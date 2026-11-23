#ifndef FIREFLY_NODE_WRITER_BASE_H_
#define FIREFLY_NODE_WRITER_BASE_H_

#include <string>

namespace firefly {

/**
 * @class WriterBase
 * @brief 发布者基类。Writer 向指定通道发送消息。
 * @warning 一个 Writer 只能写一个通道；不同 Writer 可写同一通道。
 */
class WriterBase {
 public:
  WriterBase() = default;
  virtual ~WriterBase() = default;

  virtual bool Init() = 0;
  virtual void Shutdown() = 0;

  /**
   * @brief 是否已有 Reader 订阅本通道
   */
  virtual bool HasReader() { return false; }

  virtual const std::string& GetChannelName() const = 0;
  virtual bool IsInit() const = 0;
};

}  // namespace firefly

#endif  // FIREFLY_NODE_WRITER_BASE_H_