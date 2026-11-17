#ifndef FIREFLY_NODE_READER_H_
#define FIREFLY_NODE_READER_H_

// ===========================================================================
// 回调执行模型与并发契约（阶段二契约基线；阶段三签名升级为 MessageContext）
// ===========================================================================
//
// 回调类型（阶段三）：
//   CallbackFunc<M> = std::function<CallbackResult(
//       const MessageContext&, std::shared_ptr<const M>)>
// MessageContext 含 channel、sequence（本 Reader 从 1 递增）、receive_time；
// 不含 executor 字段 —— 回调仍内联派发（见下）。
// 消息指针改为 shared_ptr<const M>：堵死"回调内改消息"的未定义行为。
// CallbackResult::kDropped 表示本条不消费，计入 Stats().dropped。
//
// 执行线程：回调在 Reader 专属的单个收线程（recv_thread_）内**内联派发**，
//   不是独立线程池，也不在调用 CreateReader 的线程执行。
// 并发与顺序：
//   - 同一 Reader：回调严格串行（单收线程逐条派发），顺序为消息到达顺序；
//   - 不同 Reader：各自的收线程并行，彼此无顺序保证。
// 回调内行为约束（违反将导致死锁或异常）：
//   - 禁止在回调内调用**本 Reader** 的 Shutdown / DeleteReader：Shutdown 会
//     join 收线程，而回调正运行在该线程上 → 自连接死锁；
//   - 禁止在回调内调用**本 Client** 的 Shutdown（对 RPC 回调同理，见 client.h）；
//   - 回调抛出的异常不会传播到任何调用方：在收线程内被捕获，记录一条带
//     channel 名的 ERROR 日志（std::exception 附 what()）后继续收下一条。
// 回调与 Observe 互斥（契约缺失 3）：
//   - 构造 Reader 时传入非空 CallbackFunc → 回调模式；回调模式下消息**只**
//     派发给回调、不写入观察队列，因此 GetAllObserved()/GetLatestObserved()/
//     GetOldestObserved() 恒为空 / nullptr，Empty() 恒为 true。
//   - 构造时传 nullptr → Observe 模式；消息入队，回调不触发。
//   - 之所以仅在文档声明而未加 kInvalidState 返回：GetAllObserved() 返回
//     std::vector 而非 Result，加码需改签名，超出范围。
// Shutdown 与 Clear（阶段三收尾）：
//   - Reader::Shutdown() 阻塞至在途回调返回（与 join 收线程配合）；
//   - 全局 firefly::Clear() 遍历登记的存活 Reader/Service 等待点，阻塞至
//     调用时刻在途的回调返回；不阻止端点未关闭时后续新回调。
// ===========================================================================

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

// 消息到达回调：收到消息后被调用（回调模式）
template <typename M0>
using CallbackFunc = std::function<CallbackResult(
    const MessageContext&, const std::shared_ptr<const M0>&)>;

/**
 * @brief 订阅者：订阅一个通道。
 *
 * 支持两种消费模式：
 *  - 回调模式：创建时传入 CallbackFunc，消息到达时被调用；
 *  - Observe 模式：不传回调，用 Observe()/GetLatestObserved() 等拉取数据。
 */
template <typename MessageT>
class Reader : public ReaderBase {
 public:
  Reader(const ReaderConfig& config,
         const CallbackFunc<MessageT>& func = nullptr,
         const std::shared_ptr<detail::InjectionContext>& ctx = nullptr)
      : config_(config), func_(func), ctx_(ctx) {}
  ~Reader() override = default;

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

  // 供 Node::CreateReader 回填 Result 的诊断信息
  ErrorCode InitErrorCode() const noexcept { return init_code_; }
  const std::string& InitErrorMessage() const noexcept { return init_message_; }

  // @post 阻塞至在途回调返回并关闭收线程；调用后本 Reader 不可再用。
  void Shutdown() override {
    detail::ReaderShutdownCore(impl_.get());
    impl_.reset();
    init_ = false;
  }

  // @threadsafe-if 在 impl_ 未被并发 Shutdown 的前提下线程安全。
  // 契约：不得与同一 Reader 的 Shutdown() 并发调用。
  void Observe() override {}  // Observe 模式下数据在收线程直接入队
  void ClearData() override { detail::ReaderClearData(impl_.get()); }

  bool Empty() const override { return detail::ReaderEmpty(impl_.get()); }
  bool HasReceived() const override {
    return detail::ReaderHasReceived(impl_.get());
  }
  double GetDelaySec() const override {
    return detail::ReaderGetDelaySec(impl_.get());
  }

  // 阶段三：实时积压由 Stats().queue_depth 提供；假 getter PendingQueueSize 已删除。
  // @threadsafe 计数与队列深度均由 core 内锁/原子量维护。
  // kDropped 回调与队列满丢弃合并计入 dropped；速率统计未实现（phase4）。
  EndpointStats Stats() const { return detail::ReaderStats(impl_.get()); }

  bool HasWriter() override { return detail::ReaderHasWriter(impl_.get()); }

  const std::string& GetChannelName() const override {
    return config_.channel_name;
  }
  bool IsInit() const override { return init_; }

  // ---------- Observe 模式 ----------

  // @threadsafe 队列由 core 内 queue_mutex_ 保护，可与收线程并发调用。
  // @note 回调模式下恒返回空 vector（见文件头"回调与 Observe 互斥"）。
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
  std::shared_ptr<MessageT> GetLatestObserved() const {
    auto all = GetAllObserved();
    return all.empty() ? nullptr : all.back();
  }
  std::shared_ptr<MessageT> GetOldestObserved() const {
    auto all = GetAllObserved();
    return all.empty() ? nullptr : all.front();
  }

  // 设置/获取缓存深度：与 QosProfile::depth 同源（阶段三统一）
  void SetHistoryDepth(std::uint32_t depth) {
    detail::ReaderSetHistoryDepth(impl_.get(), depth);
  }
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
