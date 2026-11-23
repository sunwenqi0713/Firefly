#ifndef FIREFLY_CORE_INJECTION_H_
#define FIREFLY_CORE_INJECTION_H_

// ===========================================================================
// 默认组装与注入载体（detail 层）
// ===========================================================================
// 运行时入口 Init 调用一次 BootstrapDefaultInjection，之后 Core 只通过
// ResolveInjection(ctx) 取得补齐后的依赖；Clear 调 ResetDefaultInjection。
// 测试可传入部分或全部注入字段覆盖默认后端。
// ===========================================================================

#include <memory>
#include <string>

namespace firefly {
namespace detail {

struct IEndpointFactory;
class IRegistry;

// 注入载体。字段为空时由 ResolveInjection 从运行时入口组装的默认依赖补齐；
// Core 不再自行选择后端。测试可传入完整或部分注入对象。
struct InjectionContext {
  std::shared_ptr<IEndpointFactory> endpoints;
  std::shared_ptr<IRegistry> registry;
};

// 发现中心监听地址；须在首次 Init/Start 前设置。空串表示保留当前值。
struct DiscoveryConfig {
  std::string rep_url;
  std::string pub_url;
};
void ConfigureDiscovery(const DiscoveryConfig& config);

void BootstrapDefaultInjection();
void ResetDefaultInjection();
std::shared_ptr<InjectionContext> ResolveInjection(
    const std::shared_ptr<InjectionContext>& ctx);

std::shared_ptr<IEndpointFactory> DefaultEndpointFactory();
std::shared_ptr<IRegistry> DefaultRegistry();

}  // namespace detail
}  // namespace firefly

#endif  // FIREFLY_CORE_INJECTION_H_
