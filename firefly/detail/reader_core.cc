#include "firefly/detail/endpoint_api.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "firefly/common/log.h"
#include "firefly/runtime.h"

namespace firefly {
namespace detail {
namespace {

struct ReaderCore final : ReaderHandle {
  explicit ReaderCore(std::shared_ptr<IRegistry> reg, OnBytesFunc cb)
      : registry(std::move(reg)), on_bytes(std::move(cb)) {}

  ~ReaderCore() override {
    Shutdown();
    // 兜底：若在收线程内析构（无法 join 自身），detach 避免 thread 析构
    // terminate；running_=false 已保证循环在本次回调后退出。
    if (recv_thread_.joinable()) recv_thread_.detach();
  }

  // 登记到全局等待点：Clear() 会阻塞至当前在途回调返回。
  void RegisterWaiter(const std::shared_ptr<ReaderCore>& self) {
    std::weak_ptr<ReaderCore> weak(self);
    runtime::ShutdownManager::instance().RegisterCallbackWaiter(
        std::weak_ptr<void>(self),
        [weak]() {
          if (auto locked = weak.lock()) locked->WaitForCallbacks();
        });
  }

  void WaitForCallbacks() {
    std::unique_lock<std::mutex> lock(callback_mutex_);
    callbacks_cv_.wait(lock, [this] { return in_flight_ == 0; });
  }

  void DialWriter(const std::string& url) {
    if (url.empty()) return;
    int r = socket->Dial(url);
    if (r != 0) {
      FY_WARN("reader[%s]: dial: %d", config.channel_name.c_str(), r);
      return;
    }
    FY_INFO("reader[%s]: dialed %s", config.channel_name.c_str(), url.c_str());
  }

  void RecvLoop() {
    while (running_.load(std::memory_order_acquire)) {
      std::string bytes;
      int r = socket->Recv(&bytes, 200);
      if (r == kIoTimeout) continue;
      if (r == kIoClosed) break;
      if (r != kIoOk) {
        FY_WARN("reader[%s]: recv: %d", config.channel_name.c_str(), r);
        continue;
      }
      received_.store(true, std::memory_order_relaxed);
      last_recv_sec_.store(NowSec(), std::memory_order_relaxed);
      recv_count_.fetch_add(1, std::memory_order_relaxed);

      MessageContext ctx;
      ctx.channel = config.channel_name;
      ctx.sequence = sequence_++;  // 仅本 Reader、从 1 递增
      ctx.receive_time = std::chrono::steady_clock::now();

      if (on_bytes) {
        {
          std::lock_guard<std::mutex> lock(callback_mutex_);
          ++in_flight_;
        }
        try {
          CallbackResult result = on_bytes(ctx, bytes);
          if (result == CallbackResult::kDropped) {
            dropped_count_.fetch_add(1, std::memory_order_relaxed);
          }
        } catch (const std::exception& e) {
          // 阶段二既有契约：异常不传播，记录 what() 与 channel。
          FY_ERROR("reader[%s]: callback threw: %s",
                   config.channel_name.c_str(), e.what());
        } catch (...) {
          FY_ERROR("reader[%s]: callback threw (non-std exception)",
                   config.channel_name.c_str());
        }
        {
          std::lock_guard<std::mutex> lock(callback_mutex_);
          if (--in_flight_ == 0) callbacks_cv_.notify_all();
        }
        continue;  // 回调模式不缓存
      }

      // Observe 模式：实时深度由 qos_profile.depth 决定（阶段三统一）。
      std::lock_guard<std::mutex> lock(queue_mutex_);
      std::uint32_t depth = history_depth_.load(std::memory_order_relaxed);
      if (depth == 0) depth = 1;
      while (queue_.size() >= depth) queue_.pop_front();
      queue_.push_back(std::move(bytes));
    }
    FY_INFO("reader[%s]: recv loop exit", config.channel_name.c_str());
  }

  // 幂等：显式 Shutdown 与 ~ReaderCore() 兜底都走这里。
  void Shutdown() {
    if (shutdown_done_.exchange(true)) return;
    running_.store(false, std::memory_order_release);
    // 若在收线程内触发（回调里释放最后一个 Reader 引用），join/wait 自身
    // 会死锁；running_=false 已保证循环在本次回调结束后退出。
    const bool on_recv = recv_thread_.joinable() &&
                         recv_thread_.get_id() == std::this_thread::get_id();
    if (!on_recv && recv_thread_.joinable()) recv_thread_.join();
    if (subscription_id_ != 0) {
      registry->Unsubscribe(subscription_id_);
      subscription_id_ = 0;
    }
    if (registered_) {
      registry->Unregister(RegistryKind::kReader, config.channel_name, "");
      registered_ = false;
    }
    if (socket) {
      socket->Close();
    }
    if (!on_recv) WaitForCallbacks();
  }

  static double NowSec() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
  }

  ReaderConfig config;
  std::shared_ptr<IRegistry> registry;
  OnBytesFunc on_bytes;

  std::unique_ptr<ISocket> socket;
  int subscription_id_ = 0;
  bool registered_ = false;

  std::atomic<bool> shutdown_done_{false};
  std::atomic<bool> running_{false};
  std::thread recv_thread_;
  std::uint64_t sequence_ = 1;  // 只由收线程访问

  mutable std::mutex queue_mutex_;
  std::deque<std::string> queue_;
  std::atomic<bool> received_{false};
  std::atomic<double> last_recv_sec_{-1.0};
  std::atomic<std::uint32_t> history_depth_{1};

  std::mutex callback_mutex_;
  std::condition_variable callbacks_cv_;
  std::size_t in_flight_ = 0;

  std::atomic<std::uint64_t> recv_count_{0};
  std::atomic<std::uint64_t> dropped_count_{0};
};

}  // namespace

Result<std::shared_ptr<ReaderHandle>> CreateReaderCore(
    const ReaderConfig& config, OnBytesFunc on_bytes,
    const std::shared_ptr<InjectionContext>& ctx) {
  const std::string where = "CreateReader[channel='" + config.channel_name + "']: ";
  auto injection = ctx ? ctx : DefaultInjection();
  auto transport = injection->transport ? injection->transport
                                        : DefaultTransportFactory();
  auto registry =
      injection->registry ? injection->registry : DefaultRegistry();

  auto core = std::make_shared<ReaderCore>(registry, std::move(on_bytes));
  core->config = config;
  core->history_depth_.store(config.qos_profile.depth,
                             std::memory_order_relaxed);

  core->socket = transport->Create(TransportKind::kSub);
  if (!core->socket) {
    return {nullptr, ErrorCode::kTransport,
            where + "transport create(kSub) failed"};
  }

  // 注册 reader 占位（url 为空），供 writer 的 HasReader 感知
  registry->Register(RegistryKind::kReader, config.channel_name, "");
  core->registered_ = true;

  // 订阅 writer 上下线事件；事件只带不透明 EndpointId，detail 层才解析。
  core->subscription_id_ = registry->Subscribe(
      RegistryKind::kWriter, config.channel_name,
      [weak = std::weak_ptr<ReaderCore>(core)](
          const RegistryEvent& ev) {
        auto self = weak.lock();
        if (!self || !ev.added || !self->running_.load()) return;
        if (ev.endpoint) self->DialWriter(ev.endpoint->Url());
      });

  auto urls = registry->List(RegistryKind::kWriter, config.channel_name);
  for (const auto& url : urls) core->DialWriter(url);

  core->running_.store(true, std::memory_order_release);
  // 收线程只持有 weak_ptr：释放最后一个外部 shared_ptr 时 core 能析构，
  // ~ReaderCore() 中 join 收线程并注销（修复自引用环）。
  std::weak_ptr<ReaderCore> weak = core;
  core->recv_thread_ = std::thread([weak]() {
    if (auto self = weak.lock()) self->RecvLoop();
  });
  core->RegisterWaiter(core);
  FY_INFO("reader[%s] ok, writers found: %zu", config.channel_name.c_str(),
          urls.size());
  return {core, ErrorCode::kOk, ""};
}

void ReaderShutdownCore(ReaderHandle* handle) {
  auto* core = static_cast<ReaderCore*>(handle);
  if (core) core->Shutdown();
}

void ReaderWaitForCallbacks(ReaderHandle* handle) {
  auto* core = static_cast<ReaderCore*>(handle);
  if (core) core->WaitForCallbacks();
}

void ReaderClearData(ReaderHandle* handle) {
  auto* core = static_cast<ReaderCore*>(handle);
  if (!core) return;
  std::lock_guard<std::mutex> lock(core->queue_mutex_);
  core->queue_.clear();
}

bool ReaderEmpty(const ReaderHandle* handle) {
  auto* core = static_cast<const ReaderCore*>(handle);
  if (!core) return true;
  std::lock_guard<std::mutex> lock(core->queue_mutex_);
  return core->queue_.empty();
}

bool ReaderHasReceived(const ReaderHandle* handle) {
  auto* core = static_cast<const ReaderCore*>(handle);
  return core && core->received_.load(std::memory_order_relaxed);
}

double ReaderGetDelaySec(const ReaderHandle* handle) {
  auto* core = static_cast<const ReaderCore*>(handle);
  if (!core || !core->received_.load(std::memory_order_relaxed)) return -1.0;
  double last = core->last_recv_sec_.load(std::memory_order_relaxed);
  if (last < 0) return -1.0;
  return ReaderCore::NowSec() - last;
}

std::vector<std::string> ReaderGetAllBytes(const ReaderHandle* handle) {
  std::vector<std::string> out;
  auto* core = static_cast<const ReaderCore*>(handle);
  if (!core) return out;
  std::lock_guard<std::mutex> lock(core->queue_mutex_);
  out.assign(core->queue_.begin(), core->queue_.end());
  return out;
}

void ReaderSetHistoryDepth(ReaderHandle* handle, std::uint32_t depth) {
  auto* core = static_cast<ReaderCore*>(handle);
  if (!core) return;
  if (depth == 0) depth = 1;
  core->history_depth_.store(depth, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(core->queue_mutex_);
  while (core->queue_.size() > depth) core->queue_.pop_front();
}

std::uint32_t ReaderGetHistoryDepth(const ReaderHandle* handle) {
  auto* core = static_cast<const ReaderCore*>(handle);
  return core ? core->history_depth_.load(std::memory_order_relaxed) : 0;
}

bool ReaderHasWriter(const ReaderHandle* handle) {
  auto* core = static_cast<const ReaderCore*>(handle);
  if (!core) return false;
  return core->registry->Has(RegistryKind::kWriter, core->config.channel_name);
}

EndpointStats ReaderStats(const ReaderHandle* handle) {
  EndpointStats stats;
  auto* core = static_cast<const ReaderCore*>(handle);
  if (!core) return stats;
  stats.recv = core->recv_count_.load(std::memory_order_relaxed);
  stats.dropped = core->dropped_count_.load(std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(core->queue_mutex_);
    stats.queue_depth = static_cast<std::uint64_t>(core->queue_.size());
  }
  return stats;
}

}  // namespace detail
}  // namespace firefly
