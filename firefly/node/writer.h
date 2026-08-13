#ifndef FIREFLY_NODE_WRITER_H_
#define FIREFLY_NODE_WRITER_H_

#include <memory>
#include <string>

#include "firefly/detail/handles.h"
#include "firefly/node/channel_config.h"
#include "firefly/node/writer_base.h"

namespace firefly {

namespace detail {

// 不透明实现句柄：具体内容由底层传输层持有（定义于 detail/handles.h）

// —— 以下工厂/操作函数均为“接口声明”，具体实现待后续补充 ——
template <typename MessageT>
std::shared_ptr<WriterHandle> CreateWriter(const ChannelConfig& config);

template <typename MessageT>
bool WriterWrite(WriterHandle* handle, const std::shared_ptr<MessageT>& msg);

template <typename MessageT>
bool WriterHasReader(const WriterHandle* handle);

}  // namespace detail

/**
 * @class Writer<MessageT>
 * @brief 发布者：向指定通道发布类型为 MessageT 的消息。
 *
 * @tparam MessageT 消息类型，必须是 google::protobuf::Message 派生类
 */
template <typename MessageT>
class Writer : public WriterBase {
 public:
  explicit Writer(const ChannelConfig& config) : config_(config) {}
  ~Writer() override = default;

  bool Init() override {
    if (init_) {
      return true;
    }
    impl_ = detail::CreateWriter<MessageT>(config_);
    init_ = impl_ != nullptr;
    return init_;
  }

  void Shutdown() override {
    impl_.reset();
    init_ = false;
  }

  /**
   * @brief 发布一条消息（值语义，内部拷贝为 shared_ptr）
   */
  bool Write(const MessageT& msg) {
    auto msg_ptr = std::make_shared<MessageT>(msg);
    return Write(msg_ptr);
  }

  /**
   * @brief 发布一条消息（共享指针语义）
   */
  bool Write(const std::shared_ptr<MessageT>& msg_ptr) {
    return detail::WriterWrite<MessageT>(impl_.get(), msg_ptr);
  }

  bool HasReader() override {
    return detail::WriterHasReader<MessageT>(impl_.get());
  }

  const std::string& GetChannelName() const override {
    return config_.channel_name;
  }

  bool IsInit() const override { return init_; }

 private:
  ChannelConfig config_;
  std::shared_ptr<detail::WriterHandle> impl_;
  bool init_ = false;
};

}  // namespace firefly

#include "firefly/detail/writer_impl.h"

#endif  // FIREFLY_NODE_WRITER_H_