#include "firefly/init.h"

#include "firefly/common/log.h"
#include "firefly/detail/discovery.h"
#include "firefly/runtime.h"

namespace firefly {

bool Init(const char* binary_name) {
  bool ok = runtime::ShutdownManager::instance().Init(binary_name);
  if (!ok) {
    return false;
  }
  // 注册中心惰性启动：第一次 Register/List/Subscribe 时才真正 bind/dial
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
  discovery::Registry::instance().Stop();
  // 阶段三：等待此刻仍在途的端点回调返回（Reader/Service），再清全局状态。
  // 注意：全局关停不关闭端点线程，Clear 返回后新消息仍可能触发新回调。
  runtime::ShutdownManager::instance().WaitForInFlightCallbacks();
  runtime::ShutdownManager::instance().Clear();
  FY_INFO("firefly runtime cleared");
}

}  // namespace firefly