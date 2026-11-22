#include "firefly/detail/interfaces.h"

#include <cstdlib>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "firefly/detail/backend_access.h"
#include "firefly/detail/discovery.h"

namespace firefly {
namespace detail {
namespace {

class NngRegistry final : public IRegistry {
 public:
  EndpointRecordId Register(const EndpointRecord& record) override {
    return discovery::Registry::instance().Register(record);
  }
  bool Unregister(EndpointRecordId id) override {
    return discovery::Registry::instance().Unregister(id);
  }
  std::vector<EndpointRecord> Query(
      RegistryKind kind, const std::string& name,
      const std::string& backend = std::string()) override {
    return discovery::Registry::instance().Query(kind, name, backend);
  }
  bool Has(RegistryKind kind, const std::string& name) override {
    return discovery::Registry::instance().Has(kind, name);
  }
  int Watch(RegistryKind kind, const std::string& name,
            WatchCallback&& cb) override {
    return discovery::Registry::instance().Watch(kind, name, std::move(cb));
  }
  void Unwatch(int id) override {
    discovery::Registry::instance().Unwatch(id);
  }
  void Clear() override { discovery::Registry::instance().Stop(); }
};

std::mutex g_assembly_mutex;
DiscoveryConfig g_discovery_config;
std::shared_ptr<InjectionContext> g_default_ctx;

std::string EnvValue(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr ? std::string(value) : std::string();
}

// 调用前持有 g_assembly_mutex。
void BootstrapLocked() {
  DiscoveryConfig config = g_discovery_config;
  const std::string rep = EnvValue("FIREFLY_DISCOVERY_REP_URL");
  const std::string pub = EnvValue("FIREFLY_DISCOVERY_PUB_URL");
  if (!rep.empty()) config.rep_url = rep;
  if (!pub.empty()) config.pub_url = pub;
  discovery::Registry::Configure(config.rep_url, config.pub_url);
  g_default_ctx = std::make_shared<InjectionContext>(
      InjectionContext{DefaultEndpointFactory(), DefaultRegistry()});
}

}  // namespace

void ConfigureDiscovery(const DiscoveryConfig& config) {
  std::lock_guard<std::mutex> lock(g_assembly_mutex);
  if (!config.rep_url.empty()) g_discovery_config.rep_url = config.rep_url;
  if (!config.pub_url.empty()) g_discovery_config.pub_url = config.pub_url;
}

void BootstrapDefaultInjection() {
  std::lock_guard<std::mutex> lock(g_assembly_mutex);
  BootstrapLocked();
}

void ResetDefaultInjection() {
  std::shared_ptr<InjectionContext> old;
  {
    std::lock_guard<std::mutex> lock(g_assembly_mutex);
    old = std::move(g_default_ctx);
  }
  if (old && old->registry) old->registry->Clear();
}

std::shared_ptr<InjectionContext> DefaultInjection() {
  std::lock_guard<std::mutex> lock(g_assembly_mutex);
  if (!g_default_ctx) BootstrapLocked();
  return g_default_ctx;
}

std::shared_ptr<InjectionContext> ResolveInjection(
    const std::shared_ptr<InjectionContext>& ctx) {
  auto defaults = DefaultInjection();
  auto resolved = std::make_shared<InjectionContext>();
  resolved->endpoints =
      (ctx && ctx->endpoints) ? ctx->endpoints : defaults->endpoints;
  resolved->registry =
      (ctx && ctx->registry) ? ctx->registry : defaults->registry;
  return resolved;
}

std::shared_ptr<IEndpointFactory> DefaultEndpointFactory() {
  static auto factory = MakeNngEndpointFactory();
  return factory;
}

std::shared_ptr<IRegistry> MakeNngRegistry() {
  return std::make_shared<NngRegistry>();
}

std::shared_ptr<IRegistry> DefaultRegistry() {
  static auto registry = MakeNngRegistry();
  return registry;
}

}  // namespace detail
}  // namespace firefly
