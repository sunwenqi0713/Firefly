#include "firefly/detail/endpoint_api.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "firefly/common/log.h"
#include "firefly/detail/rpc_frame.h"
#include "firefly/runtime.h"

namespace firefly {
namespace detail {
namespace {

struct ServiceCore final : ServiceHandle {
  ServiceCore(std::shared_ptr<IRegistry> reg, OnRequestFunc cb)
      : registry(std::move(reg)), on_request(std::move(cb)) {}

  ~ServiceCore() override {
    Shutdown();
    // 兜底：仅当 core 在 worker 线程内析构（回调线程触发停机且外部引用已
    // 全部释放）时线程仍 joinable 且无法 join 自身，detach 避免 terminate。
    // 正常路径 Shutdown 已 join，joinable 为 false，不会走到这里。
    if (worker_.joinable()) worker_.detach();
  }

  void RegisterWaiter(const std::shared_ptr<ServiceCore>& self) {
    std::weak_ptr<ServiceCore> weak(self);
    runtime::ShutdownManager::instance().RegisterCallbackWaiter(
        std::weak_ptr<void>(self), [weak]() {
          if (auto locked = weak.lock()) locked->WaitForCallbacks();
        });
  }

  void WaitForCallbacks() {
    std::unique_lock<std::mutex> lock(callback_mutex_);
    callbacks_cv_.wait(lock, [this] { return in_flight_ == 0; });
  }

  void WorkerLoop() {
    while (running_.load(std::memory_order_acquire)) {
      std::string req_bytes;
      int r = socket->Recv(&req_bytes, 200);
      if (r == kIoTimeout) continue;
      if (r == kIoClosed) break;
      if (r != kIoOk) {
        FY_WARN("service[%s]: recv: %d", service_name.c_str(), r);
        continue;
      }
      recv_count_.fetch_add(1, std::memory_order_relaxed);

      ErrorCode code = ErrorCode::kSerialization;
      std::string payload;
      {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        ++in_flight_;
      }
      try {
        payload = on_request(req_bytes, &code);
      } catch (const std::exception& e) {
        FY_ERROR("service[%s]: callback: %s", service_name.c_str(), e.what());
        code = ErrorCode::kServiceFailed;
        payload.clear();
      } catch (...) {
        FY_ERROR("service[%s]: callback threw (non-std exception)",
                 service_name.c_str());
        code = ErrorCode::kServiceFailed;
        payload.clear();
      }
      {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        if (--in_flight_ == 0) callbacks_cv_.notify_all();
      }

      std::string frame = EncodeResponse(code, payload);
      int send_r = socket->Send(frame);
      if (send_r == 0) {
        sent_count_.fetch_add(1, std::memory_order_relaxed);
      } else {
        FY_WARN("service[%s]: send: %d", service_name.c_str(), send_r);
      }
    }
    FY_INFO("service[%s]: worker exit", service_name.c_str());
  }

  // 幂等：Service::~Service() 显式 Shutdown、全局关停兜底、~ServiceCore()
  // 三条入口都走这里；exchange 保证停止接收/退出 worker/注销只执行一次。
  void Shutdown() {
    if (shutdown_done_.exchange(true)) return;
    running_.store(false, std::memory_order_release);
    // 若在 worker 线程内触发（回调里释放最后一个 Service 引用），join 自身
    // 会抛 system_error，WaitForCallbacks 会等当前回调自己返回而自锁。
    // 此时 running_=false 已保证循环在本次回调结束后退出，跳过这两步。
    const bool on_worker = worker_.joinable() &&
                           worker_.get_id() == std::this_thread::get_id();
    if (!on_worker && worker_.joinable()) worker_.join();
    if (registered_) {
      registry->Unregister(RegistryKind::kService, service_name, url);
      registered_ = false;
    }
    if (socket) socket->Close();
    if (!on_worker) WaitForCallbacks();
    // 自保活引用只在清理完成后释放：确保 join() 期间 worker 线程内的
    // weak.lock() 持有的强引用之外仍有引用，core 不会在 join 过程中析构
    // （否则析构里的 detach 与 join 并发，属未定义行为）。回调线程触发时
    // worker 自己的局部强引用仍会保活到循环退出，之后才在 worker 上析构。
    self_keepalive_.reset();
  }

  std::string node_name;
  std::string service_name;
  std::string url;
  std::shared_ptr<IRegistry> registry;
  OnRequestFunc on_request;

  std::unique_ptr<ISocket> socket;
  // 运行期自保活，由 Shutdown 在 join/等待完成后释放（打破自引用环）。
  std::shared_ptr<ServiceCore> self_keepalive_;
  bool registered_ = false;
  std::atomic<bool> shutdown_done_{false};
  std::atomic<bool> running_{false};
  std::thread worker_;

  std::mutex callback_mutex_;
  std::condition_variable callbacks_cv_;
  std::size_t in_flight_ = 0;

  std::atomic<std::uint64_t> recv_count_{0};
  std::atomic<std::uint64_t> sent_count_{0};
};

}  // namespace

Result<std::shared_ptr<ServiceHandle>> CreateServiceCore(
    const std::string& node_name, const std::string& service_name,
    OnRequestFunc on_request, const std::shared_ptr<InjectionContext>& ctx) {
  const std::string where = "CreateService[service='" + service_name + "']: ";
  auto injection = ctx ? ctx : DefaultInjection();
  auto transport = injection->transport ? injection->transport
                                        : DefaultTransportFactory();
  auto registry =
      injection->registry ? injection->registry : DefaultRegistry();

  auto core = std::make_shared<ServiceCore>(registry, std::move(on_request));
  core->self_keepalive_ = core;
  core->node_name = node_name;
  core->service_name = service_name;
  core->socket = transport->Create(TransportKind::kRep);
  if (!core->socket) {
    return {nullptr, ErrorCode::kTransport,
            where + "transport create(kRep) failed"};
  }
  int r = core->socket->BindEphemeral(&core->url);
  if (r != 0) {
    return {nullptr, ErrorCode::kTransport,
            where + "bind ephemeral port failed"};
  }
  if (registry->Register(RegistryKind::kService, service_name, core->url)) {
    core->registered_ = true;
  }
  core->running_.store(true, std::memory_order_release);
  // 工作线程只持有 weak_ptr：Service 释放最后一个外部 shared_ptr 时，core
  // 能正常析构并在 ~ServiceCore() 中 join 线程、注销服务（修复自引用环）。
  std::weak_ptr<ServiceCore> weak = core;
  core->worker_ = std::thread([weak]() {
    if (auto self = weak.lock()) self->WorkerLoop();
  });
  core->RegisterWaiter(core);
  FY_INFO("service[%s] ok at %s", service_name.c_str(), core->url.c_str());
  return {core, ErrorCode::kOk, ""};
}

void ServiceShutdownCore(ServiceHandle* handle) {
  auto* core = static_cast<ServiceCore*>(handle);
  if (core) core->Shutdown();
}

EndpointStats ServiceStats(const ServiceHandle* handle) {
  EndpointStats stats;
  auto* core = static_cast<const ServiceCore*>(handle);
  if (!core) return stats;
  stats.recv = core->recv_count_.load(std::memory_order_relaxed);
  stats.sent = core->sent_count_.load(std::memory_order_relaxed);
  return stats;
}

}  // namespace detail
}  // namespace firefly
