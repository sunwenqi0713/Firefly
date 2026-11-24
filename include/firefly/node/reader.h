#ifndef FIREFLY_NODE_READER_H_
#define FIREFLY_NODE_READER_H_

/// @file reader.h
/// @brief 订阅端点 Reader<MessageT> 与回调类型 CallbackFunc。
///
/// 回调执行模型、两种消费模式的互斥关系与关停等待语义写在本文件各声明处；
/// 架构层面的并发与执行位置总表见 docs/architecture/contracts.md §3。

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "firefly/common/log.h"
#include "firefly/common/types.h"
#include "firefly/detail/endpoint_api.h"
#include "firefly/message/message_traits.h"
#include "firefly/node/channel_config.h"
#include "firefly/node/reader_base.h"

namespace firefly {

/**
 * @brief 消息到达回调的签名。
 *
 * @tparam M0 消息类型，须为 google::protobuf::Message 的派生类。
 *
 * 消息以 shared_ptr<const M0> 传入，回调无法修改消息体；返回 kDropped 表示
 * 本条不消费，计入 Stats().dropped。
 */
template <typename M0>
using CallbackFunc = std::function<CallbackResult(
    const MessageContext&, const std::shared_ptr<const M0>&)>;

/**
 * @brief 订阅者：订阅一个通道。
 *
 * 支持两种消费模式，由创建时是否传入回调决定，二者互斥：
 * - 回调模式：消息只派发给回调，不写入观察队列，因此 GetAllObserved() 恒为空、
 *   GetLatestObserved() 恒为 nullptr、Empty() 恒为 true；
 * - Observe 模式：消息写入观察队列，以 Observe()/GetLatestObserved() 拉取。
 *
 * @tparam MessageT 消息类型，必须是 google::protobuf::Message 的派生类。
 *
 * @par 回调执行
 * 回调在本 Reader 专属的单个收线程内内联派发，不在创建它的线程上执行：
 * 同一 Reader 的回调严格串行且顺序等于消息到达顺序，不同 Reader 之间并行
 * 且无顺序保证。回调抛出的异常不会传播给任何调用方，只会记录一条带通道名
 * 的 ERROR 日志，然后继续收下一条。
 *
 * @warning 以下三种调用会自连接死锁或构成竞态，必须避免：
 *          - 在回调内调用本 Reader 的 Shutdown() 或 Node::DeleteReader()；
 *          - 在回调内调用本 Client 的 Shutdown()；
 *          - 与同一 Reader 的 Shutdown() 并发调用其他成员函数。
 *
 * @sa docs/architecture/contracts.md §3 「并发与执行位置」
 */
template <typename MessageT>
class Reader : public ReaderBase {
 public:
  /**
   * @brief 构造订阅者，此时尚未接入通道。
   *
   * @param[in] config 通道名与 QoS 配置；qos_profile.depth 决定观察队列深度。
   * @param[in] func   回调；传 nullptr 进入 Observe 模式。
   * @param[in] ctx    测试注入点；传 nullptr 使用默认后端与默认注册中心。
   * @note 正常业务路径应使用 Node::CreateReader()，它负责 Init() 并把失败
   *       原因回填到 Result 中。
   */
  Reader(const ReaderConfig& config,
         const CallbackFunc<MessageT>& func = nullptr,
         const std::shared_ptr<detail::InjectionContext>& ctx = nullptr)
      : config_(config), func_(func), ctx_(ctx) {}
  /// @brief 析构订阅者；不隐式关闭通道，需要确定停机时先调用 Shutdown()。
  ~Reader() override = default;

  /// @brief 接入通道并开始接收。
  /// @return 成功返回 true；失败返回 false，原因由 InitErrorCode() 给出。
  /// @post 成功后 IsInit() 为 true，收线程已启动。
  bool Init() override {
    if (init_) {
      return true;
    }
    detail::OnBytesFunc on_bytes;
    if (func_) {
      on_bytes = [func = func_](const MessageContext& context,
                                const std::string& bytes) -> CallbackResult {
        auto msg = std::make_shared<MessageT>();
        if (!message::MessageTraits<MessageT>::Deserialize(bytes, msg.get())) {
          FY_WARN("reader[%s]: deserialize failed (%zu bytes)",
                  context.channel.c_str(), bytes.size());
          return CallbackResult::kDropped;
        }
        return func(context, msg);
      };
    }
    auto r = detail::CreateReaderCore(config_, on_bytes, ctx_);
    init_code_ = r.code;
    init_message_ = std::move(r.message);
    if (!r) {
      return false;
    }
    impl_ = std::move(r.value);
    init_ = true;
    return true;
  }

  /// @brief Init() 失败时的错误码，供 Node::CreateReader() 回填 Result。
  /// @return 最近一次 Init() 的错误码；成功时为 kOk。
  ErrorCode InitErrorCode() const noexcept { return init_code_; }

  /// @brief Init() 失败时的定位线索。
  /// @return 最近一次 Init() 的错误信息；成功时为空串。
  const std::string& InitErrorMessage() const noexcept { return init_message_; }

  /**
   * @brief 关闭订阅端点。
   *
   * @post 收线程已退出、注册已注销，在途回调已返回；本 Reader 不可再用。
   * @warning 不得在回调内调用（见类注释）：Shutdown() 会 join 收线程，而回调
   *          正运行在该线程上。
   */
  void Shutdown() override {
    detail::ReaderShutdownCore(impl_.get());
    impl_.reset();
    init_ = false;
  }

  /// @brief 空实现：Observe 模式下消息在收线程直接入队，无需显式拉取。
  /// @note 保留以满足 ReaderBase 接口与既有调用方。
  void Observe() override {}

  /// @brief 清空观察队列中的全部缓存。
  /// @par 线程安全
  /// 队列由内部锁保护，可与收线程并发调用。
  void ClearData() override { detail::ReaderClearData(impl_.get()); }

  /// @brief 观察队列是否为空。
  /// @return 无缓存消息时返回 true；回调模式下恒为 true。
  bool Empty() const override { return detail::ReaderEmpty(impl_.get()); }

  /// @brief 自上次清空后是否收到过消息。
  bool HasReceived() const override {
    return detail::ReaderHasReceived(impl_.get());
  }

  /// @brief 距最近一次收到消息的时间间隔。
  /// @return 秒数；从未收到过消息时返回 -1。
  double GetDelaySec() const override {
    return detail::ReaderGetDelaySec(impl_.get());
  }

  /**
   * @brief 取累计统计快照。
   *
   * @return recv 为收到条数，dropped 为回调返回 kDropped 的条数（观察队列被
   *         深度截断的旧消息不计入），queue_depth 为观察队列当前积压条数。
   * @par 线程安全
   * 计数由内部原子量与队列锁维护，可从任意线程调用；不得与 Shutdown() 并发，
   * 因为 Shutdown() 会释放内部句柄。
   */
  EndpointStats Stats() const { return detail::ReaderStats(impl_.get()); }

  /// @brief 通道当前是否有 Writer 在发布。
  /// @return 有至少一个已注册的 Writer 时返回 true。
  bool HasWriter() override { return detail::ReaderHasWriter(impl_.get()); }

  /// @brief 取本 Reader 订阅的通道名。
  /// @return 构造时传入的通道名。
  const std::string& GetChannelName() const override {
    return config_.channel_name;
  }

  /// @brief 是否已成功 Init() 且未 Shutdown()。
  bool IsInit() const override { return init_; }

  // ---------- Observe 模式 ----------

  /**
   * @brief 取观察队列中的全部消息，按到达顺序排列。
   *
   * @return 已反序列化的消息列表；队列为空或处于回调模式时返回空 vector。
   * @note 反序列化失败的消息会被静默跳过，不进入返回值。
   * @par 线程安全
   * 队列由内部锁保护，可与收线程并发调用。
   */
  std::vector<std::shared_ptr<MessageT>> GetAllObserved() const {
    std::vector<std::shared_ptr<MessageT>> out;
    for (const auto& bytes : detail::ReaderGetAllBytes(impl_.get())) {
      auto msg = std::make_shared<MessageT>();
      if (message::MessageTraits<MessageT>::Deserialize(bytes, msg.get())) {
        out.push_back(msg);
      }
    }
    return out;
  }

  /// @brief 取观察队列中最新的一条消息。
  /// @return 消息指针；队列为空或处于回调模式时返回 nullptr。
  std::shared_ptr<MessageT> GetLatestObserved() const {
    auto all = GetAllObserved();
    return all.empty() ? nullptr : all.back();
  }

  /// @brief 取观察队列中最旧的一条消息。
  /// @return 消息指针；队列为空或处于回调模式时返回 nullptr。
  std::shared_ptr<MessageT> GetOldestObserved() const {
    auto all = GetAllObserved();
    return all.empty() ? nullptr : all.front();
  }

  /**
   * @brief 设置观察队列深度，与 QosProfile::depth 同源。
   *
   * @param[in] depth 保留的最近消息条数；传 0 会在队列截断时按 1 处理。
   * @post 后续入队按新深度截断，已缓存的多余消息在下次入队时被丢弃。
   */
  void SetHistoryDepth(std::uint32_t depth) {
    detail::ReaderSetHistoryDepth(impl_.get(), depth);
  }

  /// @brief 取观察队列深度。
  /// @return 当前生效的深度值，初值来自 QosProfile::depth。
  std::uint32_t GetHistoryDepth() const {
    return detail::ReaderGetHistoryDepth(impl_.get());
  }

 private:
  ReaderConfig config_;
  CallbackFunc<MessageT> func_;
  std::shared_ptr<detail::InjectionContext> ctx_;
  std::shared_ptr<detail::ReaderHandle> impl_;
  bool init_ = false;
  ErrorCode init_code_ = ErrorCode::kOk;
  std::string init_message_;
};

}  // namespace firefly

#endif  // FIREFLY_NODE_READER_H_
