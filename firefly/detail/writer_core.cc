#include "firefly/detail/endpoint_api.h"

#include <atomic>
#include <memory>
#include <string>
#include <utility>

#include "firefly/common/log.h"

namespace firefly {
namespace detail {
namespace {

struct WriterCore final : WriterHandle {
  explicit WriterCore(std::shared_ptr<IRegistry> reg) : registry(std::move(reg)) {}

  ~WriterCore() override {
    if (registered) {
      registry->Unregister(RegistryKind::kWriter, config.channel_name, url);
    }
  }

  ChannelConfig config;
  std::string url;
  std::unique_ptr<Publisher> publisher;
  std::shared_ptr<IRegistry> registry;
  bool registered = false;
  std::atomic<bool> closed_{false};
  std::atomic<std::uint64_t> sent{0};
  std::atomic<std::uint64_t> dropped{0};
};

}  // namespace

WriteStatus MapSendError(TransferStatus status) noexcept {
  switch (status) {
    case TransferStatus::kOk:
      return WriteStatus::kAccepted;
    case TransferStatus::kWouldBlock:
      return WriteStatus::kQueueFull;
    case TransferStatus::kClosed:
    case TransferStatus::kTimeout:
    case TransferStatus::kIoError:
    case TransferStatus::kInvalidArgument:
      return WriteStatus::kTransportError;
  }
  return WriteStatus::kTransportError;
}

Result<std::shared_ptr<WriterHandle>> CreateWriterCore(
    const ChannelConfig& config, const std::shared_ptr<InjectionContext>& ctx) {
  const std::string where = "CreateWriter[channel='" + config.channel_name + "']: ";
  auto injection = ctx ? ctx : DefaultInjection();
  auto endpoints = injection->endpoints ? injection->endpoints
                                       : DefaultEndpointFactory();
  auto registry =
      injection->registry ? injection->registry : DefaultRegistry();

  auto core = std::make_shared<WriterCore>(registry);
  core->config = config;
  core->publisher = endpoints->CreatePublisher();
  if (!core->publisher) {
    return {nullptr, ErrorCode::kTransport,
            where + "endpoint create(Publisher) failed"};
  }
  TransferStatus bound = core->publisher->Bind(&core->url);
  if (bound != TransferStatus::kOk) {
    FY_ERROR("writer[%s]: bind ephemeral port failed", config.channel_name.c_str());
    return {nullptr, ErrorCode::kTransport,
            where + "bind ephemeral port failed"};
  }
  if (registry->Register(RegistryKind::kWriter, config.channel_name,
                         core->url)) {
    core->registered = true;
  }
  FY_INFO("writer[%s] ok at %s", config.channel_name.c_str(), core->url.c_str());
  return {core, ErrorCode::kOk, ""};
}

bool WriterHasReader(const WriterHandle* handle) {
  auto* core = static_cast<const WriterCore*>(handle);
  if (!core || core->closed_.load(std::memory_order_acquire) || !core->publisher) {
    return false;
  }
  // PUB 即使无订阅者也会 send 成功；显式查注册中心以获得 kNoSubscriber。
  return core->registry->Has(RegistryKind::kReader, core->config.channel_name);
}

WriteStatus WriterWriteBytes(WriterHandle* handle, const std::string& bytes) {
  auto* core = static_cast<WriterCore*>(handle);
  if (!core || core->closed_.load(std::memory_order_acquire) || !core->publisher) {
    return WriteStatus::kTransportError;
  }
  TransferStatus status = core->publisher->Send(bytes);
  WriteStatus result = MapSendError(status);
  if (result == WriteStatus::kAccepted) {
    core->sent.fetch_add(1, std::memory_order_relaxed);
  } else if (result == WriteStatus::kQueueFull) {
    core->dropped.fetch_add(1, std::memory_order_relaxed);
  }
  if (status != TransferStatus::kOk) {
    FY_WARN("writer[%s]: send status=%d", core->config.channel_name.c_str(),
            static_cast<int>(status));
  }
  return result;
}

EndpointStats WriterStats(const WriterHandle* handle) {
  auto* core = static_cast<const WriterCore*>(handle);
  EndpointStats stats;
  if (!core) return stats;
  stats.sent = core->sent.load(std::memory_order_relaxed);
  stats.dropped = core->dropped.load(std::memory_order_relaxed);
  return stats;
}

void WriterShutdown(WriterHandle* handle) {
  auto* core = static_cast<WriterCore*>(handle);
  if (!core || core->closed_.exchange(true)) return;
  if (core->publisher) core->publisher->Close();
}

}  // namespace detail
}  // namespace firefly
