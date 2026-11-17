#ifndef FIREFLY_DETAIL_READER_IMPL_H_
#define FIREFLY_DETAIL_READER_IMPL_H_

#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nng/nng.h>
#include <nng/protocol/pubsub0/sub.h>

#include "firefly/common/log.h"
#include "firefly/detail/handles.h"
#include "firefly/discovery.h"
#include "firefly/message/message_traits.h"
#include "firefly/node/channel_config.h"

namespace firefly {
namespace detail {

// SUB 订阅者实现：持有一个 sub socket（按通道名订阅），
// 动态 dial 注册中心发现的全部 writer，收到消息走回调或观察队列。
template <typename MessageT>
struct ReaderImpl : public ReaderHandle {
  using QueueType = std::deque<std::shared_ptr<MessageT>>;

  ReaderConfig config;
  std::function<void(const std::shared_ptr<MessageT>&)> callback;  // 可空

  nng_socket sub_sock_{0};
  int discovery_sub_id_ = 0;
  bool registered_ = false;

  std::atomic<bool> running_{false};
  std::thread recv_thread_;

  std::mutex dialers_mutex_;
  std::vector<nng_dialer> dialers_;

  mutable std::mutex queue_mutex_;
  QueueType queue_;
  std::atomic<bool> received_{false};
  std::atomic<double> last_recv_sec_{-1.0};
  std::atomic<std::uint32_t> history_depth_{1};

  ~ReaderImpl() override { Shutdown(); }

  // 初始化（在空闲线程/调用线程执行）
  bool Init() {
    // 注册 reader 占位（url 为空），供 writer 的 HasReader 感知
    discovery::Registry::instance().Register(
        discovery::Kind::kReader, config.channel_name, "");
    registered_ = true;

    // 订阅 writer 上下线事件，动态新增 dial
    discovery_sub_id_ =
        discovery::Registry::instance().Subscribe(
            discovery::Kind::kWriter, config.channel_name,
            [this](const discovery::Event& ev) {
              if (ev.added && running_.load()) {
                DialWriter(ev.url);
              }
            });

    // 向当前已在线的 writer 建立连接
    auto urls = discovery::Registry::instance().List(
        discovery::Kind::kWriter, config.channel_name);
    for (const auto& url : urls) {
      DialWriter(url);
    }

    running_.store(true);
    recv_thread_ = std::thread([this]() { RecvLoop(); });
    FY_INFO("reader[%s] ok, writers found: %zu", config.channel_name.c_str(),
            urls.size());
    return true;
  }

  void Shutdown() {
    if (!running_.load() && sub_sock_.id == 0) return;
    running_.store(false);
    if (recv_thread_.joinable()) {
      recv_thread_.join();
    }
    if (discovery_sub_id_ != 0) {
      discovery::Registry::instance().Unsubscribe(discovery_sub_id_);
      discovery_sub_id_ = 0;
    }
    if (registered_) {
      discovery::Registry::instance().Unregister(
          discovery::Kind::kReader, config.channel_name, "");
      registered_ = false;
    }
    {
      std::lock_guard<std::mutex> lk(dialers_mutex_);
      for (nng_dialer d : dialers_) {
        if (d.id != 0) nng_dialer_close(d);
      }
      dialers_.clear();
    }
    if (sub_sock_.id != 0) {
      nng_close(sub_sock_);
      sub_sock_.id = 0;
    }
  }

  void DialWriter(const std::string& url) {
    nng_dialer d{NNG_DIALER_INITIALIZER};
    int r = nng_dial(sub_sock_, url.c_str(), &d, 0);
    if (r != 0) {
      FY_WARN("reader[%s]: dial %s: %s", config.channel_name.c_str(),
              url.c_str(), nng_strerror(r));
      return;
    }
    std::lock_guard<std::mutex> lk(dialers_mutex_);
    dialers_.push_back(d);
    FY_INFO("reader[%s]: dialed %s", config.channel_name.c_str(), url.c_str());
  }

  void RecvLoop() {
    nng_setopt_ms(sub_sock_, NNG_OPT_RECVTIMEO, 200);
    while (running_.load()) {
      void* data = nullptr;
      size_t size = 0;
      int r = nng_recv(sub_sock_, &data, &size, NNG_FLAG_ALLOC);
      if (r != 0) {
        if (r == NNG_ETIMEDOUT) continue;
        if (r == NNG_ECLOSED) break;
        FY_WARN("reader[%s]: recv: %s", config.channel_name.c_str(),
                nng_strerror(r));
        continue;
      }
      std::string bytes(static_cast<char*>(data), size);
      nng_free(data, size);

      auto msg = std::make_shared<MessageT>();
      if (!message::MessageTraits<MessageT>::Deserialize(bytes, msg.get())) {
        FY_WARN("reader[%s]: deserialize failed (%zu bytes)",
                config.channel_name.c_str(), bytes.size());
        continue;
      }
      received_.store(true);
      last_recv_sec_.store(NowSec());

      if (callback) {
        try {
          callback(msg);
        } catch (const std::exception& e) {
          // 修正①（阶段二）：异常不再静默折叠为固定串，记录 what() 与 channel。
          // 仍不向调用方传播——回调在收线程内联派发，传播会杀死收线程。
          FY_ERROR("reader[%s]: callback threw: %s",
                   config.channel_name.c_str(), e.what());
        } catch (...) {
          FY_ERROR("reader[%s]: callback threw (non-std exception)",
                   config.channel_name.c_str());
        }
        continue;  // 回调模式不缓存
      }

      // Observe 模式：缓存到队列
      std::lock_guard<std::mutex> lk(queue_mutex_);
      std::uint32_t depth = history_depth_.load();
      if (depth == 0) depth = 1;
      while (queue_.size() >= depth) queue_.pop_front();
      queue_.push_back(msg);
    }
    FY_INFO("reader[%s]: recv loop exit", config.channel_name.c_str());
  }

  static double NowSec() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
  }
};

// ---------- 接口层声明对应的操作函数 ----------

template <typename MessageT>
Result<std::shared_ptr<ReaderHandle>> CreateReader(
    const ReaderConfig& config,
    const std::function<void(const std::shared_ptr<MessageT>&)>& func) {
  static_assert(message::HasSerializer<MessageT>::value,
                "MessageT must derive from google::protobuf::Message.");

  const std::string where = "CreateReader[channel='" + config.channel_name + "']: ";

  auto impl = std::make_shared<ReaderImpl<MessageT>>();
  impl->config = config;
  impl->callback = func;
  impl->history_depth_.store(config.pending_queue_size);

  int r = nng_sub0_open(&impl->sub_sock_);
  if (r != 0) {
    FY_ERROR("reader[%s]: nng_sub0_open: %s", config.channel_name.c_str(),
             nng_strerror(r));
    return {nullptr, ErrorCode::kTransport,
            where + "nng_sub0_open failed: " + nng_strerror(r)};
  }
  r = nng_setopt(impl->sub_sock_, NNG_OPT_SUB_SUBSCRIBE, "", size_t(0));
  if (r != 0) {
    FY_ERROR("reader[%s]: subscribe all: %s", config.channel_name.c_str(),
             nng_strerror(r));
    nng_close(impl->sub_sock_);
    impl->sub_sock_.id = 0;
    return {nullptr, ErrorCode::kTransport,
            where + "SUB subscribe-all failed: " + nng_strerror(r)};
  }

  if (!impl->Init()) {
    return {nullptr, ErrorCode::kTransport, where + "ReaderImpl::Init failed"};
  }
  return {impl, ErrorCode::kOk, ""};
}

template <typename MessageT>
void ReaderShutdown(ReaderHandle* handle) {
  auto* impl = static_cast<ReaderImpl<MessageT>*>(handle);
  if (impl) impl->Shutdown();
}

template <typename MessageT>
void ReaderObserve(ReaderHandle* /*handle*/) {
  // Observe 模式下数据在收线程入队，这里无需额外动作。
}

template <typename MessageT>
void ReaderClearData(ReaderHandle* handle) {
  auto* impl = static_cast<ReaderImpl<MessageT>*>(handle);
  if (!impl) return;
  std::lock_guard<std::mutex> lk(impl->queue_mutex_);
  impl->queue_.clear();
}

template <typename MessageT>
bool ReaderEmpty(const ReaderHandle* handle) {
  auto* impl = static_cast<const ReaderImpl<MessageT>*>(handle);
  if (!impl) return true;
  std::lock_guard<std::mutex> lk(impl->queue_mutex_);
  return impl->queue_.empty();
}

template <typename MessageT>
bool ReaderHasReceived(const ReaderHandle* handle) {
  auto* impl = static_cast<const ReaderImpl<MessageT>*>(handle);
  return impl != nullptr && impl->received_.load();
}

template <typename MessageT>
double ReaderGetDelaySec(const ReaderHandle* handle) {
  auto* impl = static_cast<const ReaderImpl<MessageT>*>(handle);
  if (!impl || !impl->received_.load()) return -1.0;
  double last = impl->last_recv_sec_.load();
  if (last < 0) return -1.0;
  return ReaderImpl<MessageT>::NowSec() - last;
}

template <typename MessageT>
std::shared_ptr<MessageT> ReaderGetLatestObserved(const ReaderHandle* handle) {
  auto* impl = static_cast<const ReaderImpl<MessageT>*>(handle);
  if (!impl) return nullptr;
  std::lock_guard<std::mutex> lk(impl->queue_mutex_);
  if (impl->queue_.empty()) return nullptr;
  return impl->queue_.back();
}

template <typename MessageT>
std::shared_ptr<MessageT> ReaderGetOldestObserved(const ReaderHandle* handle) {
  auto* impl = static_cast<const ReaderImpl<MessageT>*>(handle);
  if (!impl) return nullptr;
  std::lock_guard<std::mutex> lk(impl->queue_mutex_);
  if (impl->queue_.empty()) return nullptr;
  return impl->queue_.front();
}

template <typename MessageT>
std::vector<std::shared_ptr<MessageT>> ReaderGetAllObserved(
    const ReaderHandle* handle) {
  std::vector<std::shared_ptr<MessageT>> out;
  auto* impl = static_cast<const ReaderImpl<MessageT>*>(handle);
  if (!impl) return out;
  std::lock_guard<std::mutex> lk(impl->queue_mutex_);
  out.assign(impl->queue_.begin(), impl->queue_.end());
  return out;
}

template <typename MessageT>
void ReaderSetHistoryDepth(ReaderHandle* handle, std::uint32_t depth) {
  auto* impl = static_cast<ReaderImpl<MessageT>*>(handle);
  if (!impl) return;
  if (depth == 0) depth = 1;
  impl->history_depth_.store(depth);
  std::lock_guard<std::mutex> lk(impl->queue_mutex_);
  while (impl->queue_.size() > depth) {
    impl->queue_.pop_front();
  }
}

template <typename MessageT>
std::uint32_t ReaderGetHistoryDepth(const ReaderHandle* handle) {
  auto* impl = static_cast<const ReaderImpl<MessageT>*>(handle);
  return impl ? impl->history_depth_.load() : 0;
}

template <typename MessageT>
bool ReaderHasWriter(const ReaderHandle* handle) {
  auto* impl = static_cast<const ReaderImpl<MessageT>*>(handle);
  if (!impl) return false;
  return discovery::Registry::instance().Has(
      discovery::Kind::kWriter, impl->config.channel_name);
}

}  // namespace detail
}  // namespace firefly

#endif  // FIREFLY_DETAIL_READER_IMPL_H_