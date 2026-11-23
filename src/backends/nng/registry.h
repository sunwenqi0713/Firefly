#ifndef FIREFLY_DETAIL_DISCOVERY_H_
#define FIREFLY_DETAIL_DISCOVERY_H_

// ===========================================================================
// 注册中心（detail 层，阶段三任务 1）
// ===========================================================================
// 本文件已从公共边界 firefly/discovery.h 移入 detail/：用户代码不得依赖
// Registry::Register/Unregister/List/Subscribe，也不得构造/解析端点地址。
// 测试可以通过 detail 路径 include（这是测试的特权）。
// 依赖方向：discovery.h -> interfaces.h；公共头只经 endpoint_api.h 依赖
// interfaces.h，不会传递包含本文件或 Registry 定义。
// 线程安全契约（阶段二原样保留）：
//   - Start/Stop 由 start_mutex_ 保护；Register/Unregister/List/Has 经
//     data_mutex_（主）或 req_mutex_（从）串行化；Subscribe/Unsubscribe
//     由 sub_mutex_ 保护。上述公共方法均可从多线程并发调用（@threadsafe）。
//   - 订阅回调在 sub_mutex_ 持有期间派发（NotifyLocal）。契约：禁止在订阅
//     回调内调用 Subscribe/Unsubscribe/Stop —— 会自锁死锁。派发回调不得抛出。
// ===========================================================================

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/interfaces.h"

namespace firefly {
namespace discovery {

// 兼容别名：Kind/Event 的定义已上提到 core/registry.h。
using Kind = firefly::detail::RegistryKind;
using Event = firefly::detail::RegistryEvent;
using firefly::detail::EndpointId;
using firefly::detail::EndpointRecord;
using firefly::detail::EndpointRecordId;
using firefly::detail::IRegistry;
using firefly::detail::MakeEndpointId;
using firefly::detail::RegistryKind;
using firefly::detail::WatchCallback;

const char* KindToString(Kind k);

// 轻量注册中心客户端（进程单例；detail 层可由 NngRegistry 适配成 IRegistry）。
// - 第一个成功 bind 的进程为主注册中心（REP 服务端 + PUB 广播）；
// - 其余进程为从实例（REQ 查询/注册 + SUB 接收广播）。
// 实现见 discovery.cc（基于 NNG）。记录模型与 Watch 快照语义见 interfaces.h
// 的 IRegistry 契约注释；Start/Stop 保留给发现层单测与 Clear 使用。
class Registry final : public IRegistry {
 public:
  static Registry& instance();

  // 在首次 Start 前设置发现中心 REP/PUB 地址；空串保持默认端口。
  static void Configure(const std::string& rep_url, const std::string& pub_url);

  // 实现细节（定义于 discovery.cc），仅用于 pimpl
  struct Impl;

  Registry(const Registry&) = delete;
  Registry& operator=(const Registry&) = delete;

  // 启动注册中心（幂等；失败返回 false，不抛出）
  bool Start();
  // 停止注册中心：退出服务线程并关闭所有内部 socket
  void Stop();

  // IRegistry 记录模型
  EndpointRecordId Register(const EndpointRecord& record) override;
  bool Unregister(EndpointRecordId id) override;
  std::vector<EndpointRecord> Query(
      RegistryKind kind, const std::string& name,
      const std::string& backend = std::string()) override;
  bool Has(RegistryKind kind, const std::string& name) override;
  int Watch(RegistryKind kind, const std::string& name,
            WatchCallback&& cb) override;
  void Unwatch(int id) override;
  void Clear() override { Stop(); }

 private:
  Registry();
  ~Registry();

  std::unique_ptr<Impl> impl_;
};

}  // namespace discovery
}  // namespace firefly

#endif  // FIREFLY_DETAIL_DISCOVERY_H_
