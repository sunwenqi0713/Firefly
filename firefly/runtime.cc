#include "firefly/runtime.h"

#include <atomic>
#include <chrono>
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <csignal>
#endif

#include "firefly/common/log.h"

namespace firefly {
namespace runtime {

// -------------- 信号处理 --------------
// 捕获 Ctrl+C（Windows）或 SIGINT（Linux/Posix），触发优雅关停。
#if defined(_WIN32)
BOOL WINAPI ConsoleCtrlHandler(DWORD fdwCtrlType) {
  if (fdwCtrlType == CTRL_C_EVENT || fdwCtrlType == CTRL_BREAK_EVENT ||
      fdwCtrlType == CTRL_CLOSE_EVENT || fdwCtrlType == CTRL_SHUTDOWN_EVENT) {
    ShutdownManager::instance().RequestShutdown();
    return TRUE;  // 已处理，避免被默认终止
  }
  return FALSE;
}
#else
static void SignalHandler(int) { ShutdownManager::instance().RequestShutdown(); }
#endif

ShutdownManager& ShutdownManager::instance() {
  static ShutdownManager mgr;  // 进程级单例
  return mgr;
}

bool ShutdownManager::Init(const char* binary_name) {
  (void)binary_name;
  if (inited_.exchange(true)) {
    return true;
  }
  shutdown_.store(false);

#if defined(_WIN32)
  // 仅主线程可以安装 Ctrl+C 处理器
  if (!SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE)) {
    FY_ERROR("SetConsoleCtrlHandler failed, err=%lu", GetLastError());
  }
#else
  struct sigaction sa {};
  sa.sa_handler = SignalHandler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  if (sigaction(SIGINT, &sa, nullptr) != 0 || sigaction(SIGTERM, &sa, nullptr) != 0) {
    FY_ERROR("sigaction failed");
  }
#endif
  FY_INFO("firefly runtime initialized");
  return true;
}

bool ShutdownManager::OK() const {
  return inited_.load() && !shutdown_.load();
}

bool ShutdownManager::IsShutdown() const { return shutdown_.load(); }

void ShutdownManager::RequestShutdown() {
  shutdown_.store(true);
  std::lock_guard<std::mutex> lock(cv_mutex_);
  cv_.notify_all();
}

void ShutdownManager::WaitForShutdown() {
  std::unique_lock<std::mutex> lock(cv_mutex_);
  cv_.wait(lock, [this] { return shutdown_.load(); });
}

bool ShutdownManager::WaitForShutdownFor(std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(cv_mutex_);
  return cv_.wait_for(lock, timeout, [this] { return shutdown_.load(); });
}

bool ShutdownManager::RegisterNode(const std::string& node_name) {
  std::lock_guard<std::mutex> lock(node_names_mutex_);
  return node_names_.insert(node_name).second;
}

void ShutdownManager::UnregisterNode(const std::string& node_name) {
  std::lock_guard<std::mutex> lock(node_names_mutex_);
  node_names_.erase(node_name);
}

bool ShutdownManager::HasNode(const std::string& node_name) const {
  std::lock_guard<std::mutex> lock(node_names_mutex_);
  return node_names_.count(node_name) != 0;
}

void ShutdownManager::RegisterCallbackWaiter(std::weak_ptr<void> owner,
                                             std::function<void()> waiter) {
  std::lock_guard<std::mutex> lock(callback_waiters_mutex_);
  callback_waiters_.emplace_back(std::move(owner), std::move(waiter));
}

void ShutdownManager::WaitForInFlightCallbacks() {
  std::vector<std::function<void()>> waiters;
  {
    std::lock_guard<std::mutex> lock(callback_waiters_mutex_);
    auto out = callback_waiters_.begin();
    for (auto it = callback_waiters_.begin(); it != callback_waiters_.end();
         ++it) {
      if (it->first.expired()) continue;
      waiters.push_back(it->second);
      *out++ = std::move(*it);
    }
    callback_waiters_.erase(out, callback_waiters_.end());
  }
  for (auto& waiter : waiters) {
    if (waiter) waiter();
  }
}

void ShutdownManager::Clear() {
  if (!inited_.exchange(false)) {
    return;
  }
  RequestShutdown();
}

}  // namespace runtime
}  // namespace firefly