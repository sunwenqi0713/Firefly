#ifndef FIREFLY_NODE_WRITER_H_
#define FIREFLY_NODE_WRITER_H_

// ===========================================================================
// Write 送达语义（显式契约，阶段一；本阶段不实现 ack/重传机制）
// ===========================================================================
//
// WriteStatus 的每个值的精确含义：
//   kAccepted          消息已成功交给**本地传输层**（nng PUB socket 的发送队列）。
//                      **不代表对端收到**：nng PUB/SUB 没有 ack，消息可能在
//                      网络/对端丢弃，调用方不得据此认为业务已送达。
//   kNoSubscriber      当前通道无任何 Reader（注册中心中无订阅者）；消息未发送。
//   kQueueFull         本地发送队列满（nng 返回 EAGAIN）；消息未发送。
//   kSerializationError 序列化失败（protobuf SerializeToString 返回 false）。
//   kTransportError    其他 nng 层错误；底层错误码写入日志。
//
// QosProfile::reliability 两种取值下 kAccepted 的行为差异（如实描述现状）：
//   kBestEffort：与当前 nng PUB/SUB 的实际行为一致 —— at-most-once，无 ack、
//                无重传、无持久化；kAccepted 仅表示已交给本地传输层。
//   kReliable  ：**本阶段与 kBestEffort 行为完全相同**。nng PUB/SUB 没有 ack，
//                当前实现不提供任何重传/确认机制，kAccepted 同样是 at-most-once。
//                TODO(phase2): 传输层可靠性增强后在此处更新契约；如需端到端确认，
//                现阶段请在应用层使用 Service/Client 的请求-响应模式并自行重发。
//
// 投递顺序保证：
//   同一 Writer 上的多次 Write 按调用顺序进入本地发送队列；但 nng PUB/SUB
//   **不保证跨 Writer、跨进程、跨订阅者的到达顺序**，当前实现不做任何排序。
//   需要顺序时请由业务在消息体中携带序号并在接收端重排。
//
// 状态机约束：
//   Write 只接受已 Init 且未 Shutdown 的 Writer；未初始化时返回 kTransportError。
//   两种 Write 重载语义一致，仅入参所有权不同（值语义会拷贝一次消息）。
// ===========================================================================

#include <memory>
#include <string>
#include <utility>

#include "firefly/common/types.h"
#include "firefly/detail/handles.h"
#include "firefly/node/channel_config.h"
#include "firefly/node/writer_base.h"

namespace firefly {

// 写操作结果：单独枚举，不与 ErrorCode 混用（Write 不返回细粒度 code/message）。
enum class WriteStatus {
  kAccepted,
  kNoSubscriber,
  kQueueFull,
  kSerializationError,
  kTransportError
};

// 便于灰度迁移：旧代码 `if (!writer->Write(msg))` 继续可用（kAccepted 视为成功）。
inline bool operator!(WriteStatus s) noexcept {
  return s != WriteStatus::kAccepted;
}

namespace detail {

// 不透明实现句柄：具体内容由底层传输层持有（定义于 detail/handles.h）

template <typename MessageT>
Result<std::shared_ptr<WriterHandle>> CreateWriter(const ChannelConfig& config);

template <typename MessageT>
WriteStatus WriterWrite(WriterHandle* handle,
                        const std::shared_ptr<const MessageT>& msg);

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
    auto r = detail::CreateWriter<MessageT>(config_);
    init_code_ = r.code;
    init_message_ = std::move(r.message);
    if (!r) {
      return false;
    }
    impl_ = std::move(r.value);
    init_ = true;
    return true;
  }

  // 供 Node::CreateWriter 回填 Result 的诊断信息
  ErrorCode InitErrorCode() const noexcept { return init_code_; }
  const std::string& InitErrorMessage() const noexcept { return init_message_; }

  void Shutdown() override {
    impl_.reset();
    init_ = false;
  }

  /** @brief 发布一条消息（值语义，内部拷贝为 shared_ptr） */
  WriteStatus Write(const MessageT& msg) {
    auto msg_ptr = std::make_shared<MessageT>(msg);
    return Write(msg_ptr);
  }

  /** @brief 发布一条消息（共享指针语义） */
  WriteStatus Write(const std::shared_ptr<const MessageT>& msg_ptr) {
    if (!init_ || !impl_) {
      return WriteStatus::kTransportError;  // kNotReady 语义在 Write 阶段折叠为传输错误
    }
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
  ErrorCode init_code_ = ErrorCode::kOk;
  std::string init_message_;
};

}  // namespace firefly

#include "firefly/detail/writer_impl.h"

#endif  // FIREFLY_NODE_WRITER_H_
