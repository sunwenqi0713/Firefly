#include "firefly/init.h"

#include "core/injection.h"
#include "core/runtime.h"
#include "firefly/common/log.h"

namespace firefly {

bool Init(const char* binary_name) {
  bool ok = runtime::ShutdownManager::instance().Init(binary_name);
  if (!ok) {
    return false;
  }
  // 运行时入口一次组装默认端点工厂 + NNG Registry；Core 只接收已解析依赖。
  detail::BootstrapDefaultInjection();
  // 注册中心惰性启动：第一次 Register/Query/Watch 时才真正 bind/dial
  return true;
}

bool OK() { return runtime::ShutdownManager::instance().OK(); }

bool IsShutdown() { return runtime::ShutdownManager::instance().IsShutdown(); }

void Shutdown() {
  runtime::ShutdownManager::instance().RequestShutdown();
}

void WaitForShutdown() {
  runtime::ShutdownManager::instance().WaitForShutdown();
}

bool WaitForShutdownFor(std::chrono::milliseconds timeout) {
  return runtime::ShutdownManager::instance().WaitForShutdownFor(timeout);
}

void Clear() {
  detail::ResetDefaultInjection();  // 停默认 Registry 并清空默认组装
  // 阶段三：等待此刻仍在途的端点回调返回（Reader/Service），再清全局状态。
  // 注意：全局关停不关闭端点线程，Clear 返回后新消息仍可能触发新回调。
  runtime::ShutdownManager::instance().WaitForInFlightCallbacks();
  runtime::ShutdownManager::instance().Clear();
  FY_INFO("firefly runtime cleared");
}

}  // namespace firefly