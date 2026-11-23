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
    // 兜底：仅当 core 在收线程内析构（回调触发停机且外部引用已全部释放）
    // 时线程仍 joinable 且无法 join 自身，detach 避免 terminate。
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

  void UpdatePublishers(const std::vector<EndpointRecord>& snapshot) {
    std::vector<std::string> addresses;
    for (const auto& rec : snapshot) {
      if (!rec.address.empty()) addresses.push_back(rec.address);
    }
    TransferStatus r = subscriber->UpdatePublishers(addresses);
    if (r != TransferStatus::kOk) {
      FY_WARN("reader[%s]: update publishers: %d", config.channel_name.c_str(),
              static_cast<int>(r));
    }
  }

  void RecvLoop() {
    while (running_.load(std::memory_order_acquire)) {
      // 绝对截止时间由本轮阻塞开始时确定；适配器内部每次轮询都重算剩余。
      const Deadline deadline =
          std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
      std::string bytes;
      TransferStatus status = subscriber->Receive(&bytes, deadline);
      if (status == TransferStatus::kTimeout ||
          status == TransferStatus::kWouldBlock) {
        continue;
      }
      if (status == TransferStatus::kClosed) break;
      if (status != TransferStatus::kOk) {
        FY_WARN("reader[%s]: recv: %d", config.channel_name.c_str(),
                static_cast<int>(status));
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

  // 固定顺序：停止接收 → 唤醒阻塞 I/O → join 收线程（回调线程内触发时跳过
  // self-join）→ 退订/注销 → 等待在途回调 → 释放自保活引用。
  void Shutdown() {
    if (shutdown_done_.exchange(true)) return;
    running_.store(false, std::memory_order_release);
    if (subscriber) subscriber->Close();  // 幂等，跨线程唤醒 Receive
    // 若在收线程内触发（回调里释放最后一个 Reader 引用），join/wait 自身
    // 会死锁；running_=false 已保证循环在本次回调结束后退出。
    const bool on_recv = recv_thread_.joinable() &&
                         recv_thread_.get_id() == std::this_thread::get_id();
    if (!on_recv && recv_thread_.joinable()) recv_thread_.join();
    if (watch_id_ != 0) {
      registry->Unwatch(watch_id_);
      watch_id_ = 0;
    }
    if (record_id_ != 0) {
      registry->Unregister(record_id_);
      record_id_ = 0;
    }
    if (!on_recv) WaitForCallbacks();
    // 与 ServiceCore 同理：join/wait 完成后才释放自保活引用，避免 core 在
    // 另一线程 join 期间被析构并在析构里 detach（未定义行为）。
    self_keepalive_.reset();
  }

  static double NowSec() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
  }

  ReaderConfig config;
  std::shared_ptr<IRegistry> registry;
  OnBytesFunc on_bytes;

  std::unique_ptr<Subscriber> subscriber;
  // 运行期自保活，由 Shutdown 在 join/等待完成后释放。
  std::shared_ptr<ReaderCore> self_keepalive_;
  int watch_id_ = 0;
  EndpointRecordId record_id_ = 0;

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

// 创建期的失败回滚 guard：self_keepalive_ 属于"运行期"所有权，只在所有
// 可能失败的步骤都成功、收线程已启动之后才建立。此前任何失败由 guard 调用
// Shutdown() 统一回收（关端点/注销/退订），避免 core 因自引用泄漏。
struct ReaderCreationGuard {
  std::shared_ptr<ReaderCore> core;
  bool committed = false;
  ~ReaderCreationGuard() {
    if (!committed && core) core->Shutdown();
  }
};

}  // namespace

Result<std::shared_ptr<ReaderHandle>> CreateReaderCore(
    const ReaderConfig& config, OnBytesFunc on_bytes,
    const std::shared_ptr<InjectionContext>& ctx) {
  const std::string where = "CreateReader[channel='" + config.channel_name + "']: ";
  auto injection = ResolveInjection(ctx);
  auto endpoints = injection->endpoints;
  auto registry = injection->registry;

  auto core = std::make_shared<ReaderCore>(registry, std::move(on_bytes));
  ReaderCreationGuard rollback{core};
  core->config = config;
  core->history_depth_.store(config.qos_profile.depth,
                             std::memory_order_relaxed);

  core->subscriber = endpoints->CreateSubscriber();
  if (!core->subscriber) {
    return {nullptr, ErrorCode::kTransport,
            where + "endpoint create(Subscriber) failed"};
  }

  // Reader 无监听地址也独立登记；id 由 Registry 分配，注销只认 id。
  EndpointRecord record;
  record.kind = RegistryKind::kReader;
  record.name = config.channel_name;
  core->record_id_ = registry->Register(record);
  if (core->record_id_ == 0) {
    return {nullptr, ErrorCode::kTransport,
            where + "registry register failed"};
  }

  // Watch 先回传当前一致快照，再派发后续增删；地址集合交给适配器去重建连。
  core->watch_id_ = registry->Watch(
      RegistryKind::kWriter, config.channel_name,
      [weak = std::weak_ptr<ReaderCore>(core)](
          const std::vector<EndpointRecord>& snapshot) {
        auto self = weak.lock();
        if (!self || self->shutdown_done_.load(std::memory_order_acquire)) return;
        self->UpdatePublishers(snapshot);
      });
  if (core->watch_id_ == 0) {
    return {nullptr, ErrorCode::kTransport, where + "registry watch failed"};
  }

  core->running_.store(true, std::memory_order_release);
  // 收线程只持有 weak_ptr：释放最后一个外部 shared_ptr 时 core 能析构，
  // ~ReaderCore() 中 join 收线程并注销（修复自引用环）。
  std::weak_ptr<ReaderCore> weak = core;
  core->recv_thread_ = std::thread([weak]() {
    if (auto self = weak.lock()) self->RecvLoop();
  });
  // 线程已启动，所有失败点已过：此时才建立运行期自保活并交接所有权。
  core->self_keepalive_ = core;
  rollback.committed = true;
  core->RegisterWaiter(core);
  FY_INFO("reader[%s] ok", config.channel_name.c_str());
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
