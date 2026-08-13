#include "firefly/init.h"

#include "firefly/common/log.h"
#include "firefly/discovery.h"
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

void WaitForShutdown() {
  runtime::ShutdownManager::instance().WaitForShutdown();
}

void Clear() {
  discovery::Registry::instance().Stop();
  runtime::ShutdownManager::instance().Clear();
  FY_INFO("firefly runtime cleared");
}

}  // namespace firefly