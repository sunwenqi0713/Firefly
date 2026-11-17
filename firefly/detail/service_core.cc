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

  ~ServiceCore() override { Shutdown(); }

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

  void Shutdown() {
    if (!running_.load(std::memory_order_acquire) && !socket) return;
    running_.store(false, std::memory_order_release);
    if (worker_.joinable()) worker_.join();
    if (registered_) {
      registry->Unregister(RegistryKind::kService, service_name, url);
      registered_ = false;
    }
    if (socket) socket->Close();
    WaitForCallbacks();
  }

  std::string node_name;
  std::string service_name;
  std::string url;
  std::shared_ptr<IRegistry> registry;
  OnRequestFunc on_request;

  std::unique_ptr<ISocket> socket;
  bool registered_ = false;
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
  core->worker_ = std::thread([core]() { core->WorkerLoop(); });
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
