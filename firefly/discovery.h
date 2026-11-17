#ifndef FIREFLY_DISCOVERY_H_
#define FIREFLY_DISCOVERY_H_

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace firefly {
namespace discovery {

// 注册对象类型：writer(发布者)/reader(订阅者)/service(服务)
enum class Kind { kWriter, kReader, kService };

const char* KindToString(Kind k);

// 注册表变更事件（用于订阅者动态发现新端点）
struct Event {
  Kind kind;
  std::string name;   // 通道名 / 服务名
  std::string url;    // 端点地址，reader 为占位空串
  bool added;         // true=新增, false=移除
};

// 轻量注册中心客户端（进程单例）。
// - 第一个成功 bind 的进程为主注册中心（REP 服务端 + PUB 广播）；
// - 其余进程为从实例（REQ 查询/注册 + SUB 接收广播）。
//
// 线程安全契约（阶段二，按实现现状标注，未新增锁）：
//   - Start/Stop 由 start_mutex_ 保护；Register/Unregister/List/Has 经
//     data_mutex_（主）或 req_mutex_（从）串行化；Subscribe/Unsubscribe
//     由 sub_mutex_ 保护。上述公共方法均可从多线程并发调用（@threadsafe）。
//   - 订阅回调在 sub_mutex_ 持有期间派发（NotifyLocal）。契约：禁止在订阅
//     回调内调用 Subscribe/Unsubscribe/Stop —— 会自锁死锁。派发回调不得抛出，
//     否则异常穿透到注册中心的 REP/广播线程。
// 实现见 discovery.cc（基于 NNG）。
class Registry {
 public:
  static Registry& instance();

  // 实现细节（定义于 discovery.cc），仅用于 pimpl
  struct Impl;

  Registry(const Registry&) = delete;
  Registry& operator=(const Registry&) = delete;

  // 启动注册中心（幂等；失败返回 false，不抛出）
  bool Start();
  // 停止注册中心：退出服务线程并关闭所有内部 socket
  void Stop();

  // 注册/注销：成功返回 true
  bool Register(Kind kind, const std::string& name, const std::string& url);
  bool Unregister(Kind kind, const std::string& name, const std::string& url);

  // 查询：返回 name 下所有端点（失败返回空）
  std::vector<std::string> List(Kind kind, const std::string& name);

  // name 下是否存在至少一个注册项
  bool Has(Kind kind, const std::string& name);

  // 订阅某名字的变更事件；返回订阅号，用 Unsubscribe 注销
  int Subscribe(Kind kind, const std::string& name,
                std::function<void(const Event&)>&& cb);
  void Unsubscribe(int id);

 private:
  Registry();
  ~Registry();

  std::unique_ptr<Impl> impl_;
};

}  // namespace discovery
}  // namespace firefly

#endif  // FIREFLY_DISCOVERY_H_