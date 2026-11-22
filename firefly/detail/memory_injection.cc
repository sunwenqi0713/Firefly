#include "firefly/detail/interfaces.h"

#include <mutex>
#include <string>
#include <utility>

#include "firefly/detail/backend_access.h"

namespace firefly {
namespace detail {
namespace {

std::mutex g_assembly_mutex;
DiscoveryConfig g_discovery_config;
std::shared_ptr<InjectionContext> g_default_ctx;

void BootstrapLocked() {
  g_default_ctx = std::make_shared<InjectionContext>(
      InjectionContext{DefaultEndpointFactory(), DefaultRegistry()});
}

}  // namespace

void ConfigureDiscovery(const DiscoveryConfig& config) {
  std::lock_guard<std::mutex> lock(g_assembly_mutex);
  g_discovery_config = config;
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
  static auto factory = MakeMemoryEndpointFactory();
  return factory;
}

std::shared_ptr<IRegistry> DefaultRegistry() {
  static auto registry = MakeMemoryRegistry();
  return registry;
}

}  // namespace detail
}  // namespace firefly
