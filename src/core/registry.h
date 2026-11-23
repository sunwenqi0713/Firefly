#ifndef FIREFLY_CORE_REGISTRY_H_
#define FIREFLY_CORE_REGISTRY_H_

// ===========================================================================
// 发现层 / 注册中心契约（Core ↔ 后端 Registry）
// ===========================================================================
// IRegistry 与记录模型只存在于 Core 内部：公共 API 不暴露 Registry 方法，
// 也不暴露任何 nng 类型。默认实现由构建开关选中的后端注入 object 提供
// （FIREFLY_WITH_NNG=ON 为 NNG，OFF 为内存）；两种后端实现同一组契约。
// ===========================================================================

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace firefly {
namespace detail {

// 注册对象类型：writer(发布者)/reader(订阅者)/service(服务)。
enum class RegistryKind { kWriter, kReader, kService };

// 端点地址的不透明句柄：公共 API 只传递 shared_ptr，用户无法构造/解析 URL。
// DebugString() 仅供日志与诊断，不得用作控制流输入。
class EndpointId {
 public:
  const std::string& Url() const noexcept { return url_; }  // detail-only
  std::string DebugString() const;

 private:
  friend std::shared_ptr<const EndpointId> MakeEndpointId(const std::string&);
  explicit EndpointId(std::string url) : url_(std::move(url)) {}
  std::string url_;
};

std::shared_ptr<const EndpointId> MakeEndpointId(const std::string& url);

// 注册表变更事件：端点地址以不透明句柄传递。
struct RegistryEvent {
  RegistryKind kind;
  std::string name;
  std::shared_ptr<const EndpointId> endpoint;  // reader 为空串占位
  bool added;
};

// ---------------- 发现层记录模型（第 3 批契约）----------------
// EndpointRecord{id, kind, name, backend, address} 是一次注册的不可变快照：
//   id      —— 注册成功返回的不透明整数句柄（EndpointRecordId），注销只认它；
//              同一次注册在 Watch 快照与 Query 结果里 id 不变。
//   kind/name —— 发现键；同一 kind+name 下允许多条记录（多 Writer、多 Reader、
//              多 Service 实例并存），Query 必须全部返回，不得相互覆盖。
//   backend —— 后端标识（如 "nng"/"fake"）；由 Registry 实现填充，Core 传入
//              空串表示交由注册中心决定。Query 的 backend 过滤为空串表示不过滤。
//   address —— 监听地址串；Reader 没有监听地址时以空串占位独立登记，不能被
//              同通道其他 Reader 的占位记录合并，删除其中一个不影响其余。
// 注意区分两个 id：本记录的 EndpointRecordId 是“注册句柄”；已有的 EndpointId
// 类是“地址不透明句柄”（RegistryEvent::endpoint），仅用于日志/诊断与 Core 内
// 部 dial，不参与注销。
//
// Watch(kind, name, cb) 的快照语义：
//   1) 注册回调与计算初始快照在同一个串行化域内完成，回调先收到一份一致的
//      当前记录集合（按 id 排序），不会漏掉注册之前已存在的记录；
//   2) 之后该键任何 Register/Unregister 都会再次以“该键的完整当前快照”派发；
//   3) 回调内禁止调用任何 IRegistry 方法（派发持有内部串行锁）；回调不得抛
//      出异常。Unwatch 后不再派发。
// Follower 补充契约（P1 修复）：Watch 的初始快照由 IRegistry 实现负责从
// Primary 对账，不能只依赖本进程启动后收到的 PUB 广播。
//
// Clear()/生命周期：Clear 幂等，清空所有记录与 Watch 订阅；Clear 之后到达的
// 迟到 Unregister(id) 为 no-op 并返回 true；再次 Register/Watch 可以重启发现
// 中心，且不得看到 Clear 之前的任何旧记录。Clear 同时停掉注册中心的工作线程
// 与监听端点；Register 失败不得留下线程或监听器。
//
// 线协议兼容：注册/注销命令与广播在末尾追加可选 id/backend 字段；旧版进程
// 忽略新增字段，新版进程对缺失字段按 kind+name+address 派生稳定的兼容 id，
// 且对 4 字段旧请求保留 "OK\n" 响应。新旧混跑时地址与 FFRP 帧格式均不变。
using EndpointRecordId = std::uint64_t;

// 注册中心持有的单条端点记录。Register 入参允许 id==0、backend 为空，由
// Registry 分配 id 并填入默认 backend；其余字段由调用方给出。
struct EndpointRecord {
  EndpointRecordId id = 0;
  RegistryKind kind = RegistryKind::kWriter;
  std::string name;
  std::string backend;
  std::string address;
};

using WatchCallback =
    std::function<void(const std::vector<EndpointRecord>& snapshot)>;

// ---------------- IRegistry ----------------
class IRegistry {
 public:
  virtual ~IRegistry() = default;
  // 成功返回非零 id；失败返回 0。注册不替换已有记录。
  virtual EndpointRecordId Register(const EndpointRecord& record) = 0;
  // 按 id 精确注销；Clear 后的迟到调用返回 true 且不做任何事。
  virtual bool Unregister(EndpointRecordId id) = 0;
  // 按 kind+name 返回全部记录；backend 为空串表示不过滤。
  virtual std::vector<EndpointRecord> Query(
      RegistryKind kind, const std::string& name,
      const std::string& backend = std::string()) = 0;
  virtual bool Has(RegistryKind kind, const std::string& name) = 0;
  // 注册即回传当前一致快照，此后每次变更再回传该键的完整快照。
  virtual int Watch(RegistryKind kind, const std::string& name,
                    WatchCallback&& cb) = 0;
  virtual void Unwatch(int id) = 0;
  // 幂等：停线程/监听器，清空记录与订阅；可再次 Register/Watch 重启。
  virtual void Clear() = 0;
};

}  // namespace detail
}  // namespace firefly

#endif  // FIREFLY_CORE_REGISTRY_H_
