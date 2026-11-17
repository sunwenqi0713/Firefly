#ifndef FIREFLY_NODE_READER_H_
#define FIREFLY_NODE_READER_H_

// ===========================================================================
// 回调执行模型与并发契约（阶段二：契约基线，签名未改）
// ===========================================================================
//
// 回调类型 CallbackFunc<M> = std::function<void(const std::shared_ptr<M>&)>。
// 本阶段不改签名（带线程上下文/返回状态的 MessageContext 改造属第三阶段）。
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
//     "静默吞掉"是阶段一旧状，阶段二已改为显式日志，但仍不传播。
// 回调与 Observe 互斥（契约缺失 3）：
//   - 构造 Reader 时传入非空 CallbackFunc → 回调模式；回调模式下消息**只**
//     派发给回调、不写入观察队列，因此 GetAllObserved()/GetLatestObserved()/
//     GetOldestObserved() 恒为空 / nullptr，Empty() 恒为 true。这是设计行为，
//     不是数据丢失；两种模式需在创建时二选一，不能运行期混用。
//   - 构造时传 nullptr → Observe 模式；消息入队，回调不触发。
//   - 之所以仅在文档声明而未加 kInvalidState 返回：GetAllObserved() 返回
//     std::vector 而非 Result，加码需改签名，超出本阶段"不改回调签名"范围。
// ===========================================================================

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "firefly/common/types.h"
#include "firefly/detail/handles.h"
#include "firefly/node/channel_config.h"
#include "firefly/node/reader_base.h"

namespace firefly {

// 消息到达回调：收到消息后被调用（回调模式）
template <typename M0>
using CallbackFunc = std::function<void(const std::shared_ptr<M0>&)>;

namespace detail {

// 不透明实现句柄（定义于 detail/handles.h）

// —— 以下工厂与操作函数为接口声明，尚未实现 ——
template <typename MessageT>
Result<std::shared_ptr<ReaderHandle>> CreateReader(
    const ReaderConfig& config, const CallbackFunc<MessageT>& func);

template <typename MessageT>
void ReaderShutdown(ReaderHandle* handle);

template <typename MessageT>
void ReaderObserve(ReaderHandle* handle);

template <typename MessageT>
void ReaderClearData(ReaderHandle* handle);

template <typename MessageT>
bool ReaderEmpty(const ReaderHandle* handle);

template <typename MessageT>
bool ReaderHasReceived(const ReaderHandle* handle);

template <typename MessageT>
double ReaderGetDelaySec(const ReaderHandle* handle);

template <typename MessageT>
std::shared_ptr<MessageT> ReaderGetLatestObserved(const ReaderHandle* handle);

template <typename MessageT>
std::shared_ptr<MessageT> ReaderGetOldestObserved(const ReaderHandle* handle);

template <typename MessageT>
std::vector<std::shared_ptr<MessageT>> ReaderGetAllObserved(
    const ReaderHandle* handle);

template <typename MessageT>
void ReaderSetHistoryDepth(ReaderHandle* handle, std::uint32_t depth);

template <typename MessageT>
std::uint32_t ReaderGetHistoryDepth(const ReaderHandle* handle);

template <typename MessageT>
bool ReaderHasWriter(const ReaderHandle* handle);

}  // namespace detail

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
  Reader(const ReaderConfig& config, const CallbackFunc<MessageT>& func = nullptr)
      : config_(config), func_(func) {}
  ~Reader() override = default;

  bool Init() override {
    if (init_) {
      return true;
    }
    auto r = detail::CreateReader<MessageT>(config_, func_);
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

  void Shutdown() override {
    detail::ReaderShutdown<MessageT>(impl_.get());
    impl_.reset();
    init_ = false;
  }

  // @threadsafe-if 在 impl_ 未被并发 Shutdown 的前提下线程安全：底层队列由
  // queue_mutex_ 保护，但 Shutdown() 会 reset impl_，与读取 impl_.get() 存在
  // 生命周期竞态。契约：不得与同一 Reader 的 Shutdown() 并发调用。
  void Observe() override { detail::ReaderObserve<MessageT>(impl_.get()); }
  void ClearData() override { detail::ReaderClearData<MessageT>(impl_.get()); }

  bool Empty() const override {
    return detail::ReaderEmpty<MessageT>(impl_.get());
  }
  bool HasReceived() const override {
    return detail::ReaderHasReceived<MessageT>(impl_.get());
  }
  double GetDelaySec() const override {
    return detail::ReaderGetDelaySec<MessageT>(impl_.get());
  }
  std::uint32_t PendingQueueSize() const override {
    return config_.pending_queue_size;
  }

  bool HasWriter() override {
    return detail::ReaderHasWriter<MessageT>(impl_.get());
  }

  const std::string& GetChannelName() const override {
    return config_.channel_name;
  }
  bool IsInit() const override { return init_; }

  // ---------- Observe 模式 ----------

  // 最新一条已观察消息（无则返回 nullptr）
  std::shared_ptr<MessageT> GetLatestObserved() const {
    return detail::ReaderGetLatestObserved<MessageT>(impl_.get());
  }
  // 最旧一条已观察消息（无则返回 nullptr）
  std::shared_ptr<MessageT> GetOldestObserved() const {
    return detail::ReaderGetOldestObserved<MessageT>(impl_.get());
  }
  // @threadsafe 队列由 queue_mutex_ 保护，可与收线程并发调用。
  // @note 回调模式下恒返回空 vector（见文件头"回调与 Observe 互斥"）。
  std::vector<std::shared_ptr<MessageT>> GetAllObserved() const {
    return detail::ReaderGetAllObserved<MessageT>(impl_.get());
  }

  // 设置/获取缓存深度
  void SetHistoryDepth(std::uint32_t depth) {
    detail::ReaderSetHistoryDepth<MessageT>(impl_.get(), depth);
  }
  std::uint32_t GetHistoryDepth() const {
    return detail::ReaderGetHistoryDepth<MessageT>(impl_.get());
  }

 private:
  ReaderConfig config_;
  CallbackFunc<MessageT> func_;
  std::shared_ptr<detail::ReaderHandle> impl_;
  bool init_ = false;
  ErrorCode init_code_ = ErrorCode::kOk;
  std::string init_message_;
};

}  // namespace firefly

#include "firefly/detail/reader_impl.h"

#endif  // FIREFLY_NODE_READER_H_