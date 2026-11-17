#ifndef FIREFLY_RUNTIME_H_
#define FIREFLY_RUNTIME_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <set>
#include <string>

namespace firefly {
namespace runtime {

// 全局运行状态：Init / OK / Shutdown 标志与等待原语。
// 全部为单例（进程内只有一个实例），实现见 runtime.cc。
class ShutdownManager {
 public:
  static ShutdownManager& instance();

  ShutdownManager(const ShutdownManager&) = delete;
  ShutdownManager& operator=(const ShutdownManager&) = delete;

  bool Init(const char* binary_name);
  void Clear();

  // 进程级关停标志：置位后 OK() 返回 false，收/发循环应尽快退出
  void RequestShutdown();
  bool IsShutdown() const;
  bool OK() const;

  // 阻塞至 RequestShutdown() 被触发（用于 main 主线程等待信号）
  void WaitForShutdown();
  // 带超时的等待：返回 true=已关停，false=超时。供优雅关停设置退出期限。
  bool WaitForShutdownFor(std::chrono::milliseconds timeout);

  // 进程内节点名注册表（阶段二：CreateNode 查重，返回 kDuplicate）。
  // 复用本单例，避免第二套进程级状态机。true=名字此前未占用（已登记）。
  bool RegisterNode(const std::string& node_name);
  void UnregisterNode(const std::string& node_name);
  bool HasNode(const std::string& node_name) const;

 private:
  ShutdownManager() = default;

  std::atomic<bool> shutdown_{false};
  std::atomic<bool> inited_{false};
  std::mutex cv_mutex_;
  std::condition_variable cv_;

  mutable std::mutex node_names_mutex_;
  std::set<std::string> node_names_;
};

}  // namespace runtime
}  // namespace firefly

#endif  // FIREFLY_RUNTIME_H_