#ifndef FIREFLY_NODE_WRITER_H_
#define FIREFLY_NODE_WRITER_H_

/// @file writer.h
/// @brief 发布端点 Writer<MessageT> 的声明。
///
/// 送达语义只在 Write() 的注释里说明一次；架构层面的所有权、关停次序与
/// 错误映射见 docs/architecture/contracts.md。

#include <memory>
#include <string>
#include <utility>

#include "firefly/common/types.h"
#include "firefly/detail/endpoint_api.h"
#include "firefly/message/message_traits.h"
#include "firefly/node/channel_config.h"
#include "firefly/node/writer_base.h"

namespace firefly {

/**
 * @brief 发布者：向指定通道发布类型为 MessageT 的消息。
 *
 * 由 Node::CreateWriter() 创建并返回 shared_ptr；Node 析构不销毁端点，
 * 须由持有者调用 Node::DeleteWriter() 或释放最后一个引用。
 *
 * @tparam MessageT 消息类型，必须是 google::protobuf::Message 的派生类。
 * @sa docs/architecture/contracts.md 「发布送达语义」
 */
template <typename MessageT>
class Writer : public WriterBase {
 public:
  /**
   * @brief 构造发布者，此时尚未接入通道。
   *
   * @param[in] config 通道名与 QoS 配置。
   * @param[in] ctx    测试注入点；传 nullptr 使用默认后端与默认注册中心。
   * @note 正常业务路径应使用 Node::CreateWriter()，它负责 Init() 并把失败
   *       原因回填到 Result 中。
   */
  explicit Writer(const ChannelConfig& config,
                  const std::shared_ptr<detail::InjectionContext>& ctx = nullptr)
      : config_(config), ctx_(ctx) {}
  /// @brief 析构发布者；不隐式关闭通道，需要确定停机时先调用 Shutdown()。
  ~Writer() override = default;

  /// @brief 接入通道并完成注册。
  /// @return 成功返回 true；失败返回 false，原因由 InitErrorCode() 给出。
  /// @post 成功后 IsInit() 为 true，可开始 Write()。
  bool Init() override {
    if (init_) {
      return true;
    }
    auto r = detail::CreateWriterCore(config_, ctx_);
    init_code_ = r.code;
    init_message_ = std::move(r.message);
    if (!r) {
      return false;
    }
    impl_ = std::move(r.value);
    init_ = true;
    return true;
  }

  /// @brief Init() 失败时的错误码，供 Node::CreateWriter() 回填 Result。
  /// @return 最近一次 Init() 的错误码；成功时为 kOk。
  ErrorCode InitErrorCode() const noexcept { return init_code_; }

  /// @brief Init() 失败时的定位线索。
  /// @return 最近一次 Init() 的错误信息；成功时为空串。
  const std::string& InitErrorMessage() const noexcept { return init_message_; }

  /**
   * @brief 关闭发布端点。
   *
   * @post 本 Writer 不可再用：Write() 返回 kTransportError，HasReader() 返回 false。
   * @note 非幂等副作用仅限释放内部句柄（重复调用安全）；需要重新发布时
   *       应由 Node::CreateWriter() 新建。
   * @warning 不得与同一 Writer 的 Write() 并发调用。
   */
  void Shutdown() override {
    detail::WriterShutdown(impl_.get());
    impl_.reset();
    init_ = false;
  }

  /**
   * @brief 向本通道发布一条消息，按值传入并在内部拷贝一次。
   *
   * @param[in] msg 待发布消息。
   * @return 本次写入的受理结果，取值含义见接收 shared_ptr 的重载。
   */
  WriteStatus Write(const MessageT& msg) {
    auto msg_ptr = std::make_shared<MessageT>(msg);
    return Write(msg_ptr);
  }

  /**
   * @brief 向本通道发布一条消息，共享指针语义，不额外拷贝消息体。
   *
   * kAccepted 只表示消息已交给本地传输层，**不表示对端已收到**：传输不提供
   * 确认，消息可能在对端或网络中被丢弃。QosProfile::reliability 的两种取值
   * 当前行为相同，均不提供重传或持久化；需要端到端确认时请改用
   * Service/Client 的请求-应答模式并由业务自行重发。
   *
   * @param[in] msg_ptr 待发布消息；为空指针时行为未定义，调用方须保证非空。
   * @return 本次写入的受理结果。
   * @retval kAccepted           已交给本地传输层，不构成送达承诺。
   * @retval kNoSubscriber       通道当前无 Reader，消息未发送。
   * @retval kQueueFull          本地发送队列已满，消息未发送。
   * @retval kSerializationError 序列化失败，消息未发送。
   * @retval kTransportError     Writer 未初始化、已关停，或底层传输失败。
   *
   * @pre Writer 已成功 Init()。
   * @post 失败时 Writer 仍可用，调用方可直接重试。
   *
   * @par 线程安全
   * 可与同一 Writer 的其他 Write() 并发；不得与 Shutdown() 并发。
   *
   * @note 同一 Writer 的多次 Write() 按调用顺序进入本地发送队列；跨 Writer、
   *       跨进程、跨订阅者均不保证到达顺序，需要顺序时由业务在消息体中携带
   *       序号并在接收端重排。
   */
  WriteStatus Write(const std::shared_ptr<const MessageT>& msg_ptr) {
    if (!init_ || !impl_) {
      return WriteStatus::kTransportError;
    }
    if (!detail::WriterHasReader(impl_.get())) {
      return WriteStatus::kNoSubscriber;
    }
    std::string bytes;
    if (!message::MessageTraits<MessageT>::Serialize(*msg_ptr, &bytes)) {
      return WriteStatus::kSerializationError;
    }
    return detail::WriterWriteBytes(impl_.get(), bytes);
  }

  /**
   * @brief 取累计统计快照。
   *
   * @return sent 为成功交给传输层的条数，dropped 为本地队列满丢弃的条数，
   *         queue_depth 对 Writer 恒为 0。
   * @par 线程安全
   * 计数由内部原子量维护，可从任意线程调用；不得与 Shutdown() 并发，
   * 因为 Shutdown() 会释放内部句柄。
   */
  EndpointStats Stats() const { return detail::WriterStats(impl_.get()); }

  /// @brief 通道当前是否有 Reader。
  /// @return 有至少一个已注册的 Reader 时返回 true；关停后恒为 false。
  bool HasReader() override { return detail::WriterHasReader(impl_.get()); }

  /// @brief 取本 Writer 绑定的通道名。
  /// @return 构造时传入的通道名。
  const std::string& GetChannelName() const override {
    return config_.channel_name;
  }

  /// @brief 是否已成功 Init() 且未 Shutdown()。
  bool IsInit() const override { return init_; }

 private:
  ChannelConfig config_;
  std::shared_ptr<detail::InjectionContext> ctx_;
  std::shared_ptr<detail::WriterHandle> impl_;
  bool init_ = false;
  ErrorCode init_code_ = ErrorCode::kOk;
  std::string init_message_;
};

}  // namespace firefly

#endif  // FIREFLY_NODE_WRITER_H_
