#ifndef FIREFLY_NODE_READER_H_
#define FIREFLY_NODE_READER_H_

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
  // 取全部已观察消息
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