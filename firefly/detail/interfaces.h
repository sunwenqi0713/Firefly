#ifndef FIREFLY_DETAIL_INTERFACES_H_
#define FIREFLY_DETAIL_INTERFACES_H_

// ===========================================================================
// 传输与注册中心的注入接口（detail 层，阶段三任务 4）
// ===========================================================================
// 这些接口只存在于 detail 层：公共 API 不暴露任何 nng 类型，也不暴露
// Registry 的具体方法。fake 注入发生在 CreateXxx 工厂层：CreateNode 的注入
// 重载把 InjectionContext 传给端点；端点仍由外部 shared_ptr 独立持有（沿用
// 阶段二方案 b），Node 生命周期不控制端点存亡。
// 默认实现由 transport_nng.cc 提供，行为与阶段二完全一致。
// ===========================================================================

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace firefly {
namespace detail {

// 角色接口的统一 I/O 状态：NNG 常量不进入 Core，所有后端错误在此收口。
// 语义表见 endpoint_api.h 顶部注释。
enum class TransferStatus {
  kOk,
  kTimeout,
  kClosed,
  kWouldBlock,
  kIoError,
  kInvalidArgument,
};

using Deadline = std::chrono::steady_clock::time_point;

// 不透明应答句柄：Core 只回传，不解析内容（见 endpoint_api.h 契约表）。
class ReplyToken {
 public:
  virtual ~ReplyToken() = default;
};

// ---------------- 数据传输的四个角色接口 ----------------

class Publisher {
 public:
  virtual ~Publisher() = default;
  virtual TransferStatus Bind(std::string* endpoint) = 0;
  virtual TransferStatus Send(const std::string& bytes) = 0;
  virtual void Close() = 0;
};

class Subscriber {
 public:
  virtual ~Subscriber() = default;
  virtual TransferStatus Connect(const std::string& endpoint) = 0;
  virtual TransferStatus Receive(std::string* bytes, Deadline deadline) = 0;
  virtual void Close() = 0;
};

class RpcRequester {
 public:
  virtual ~RpcRequester() = default;
  // deadline 为绝对时间点；连接/发送/接收各阻塞点前必须重算剩余预算。
  virtual TransferStatus Exchange(const std::string& endpoint,
                                  const std::string& request, Deadline deadline,
                                  std::string* response) = 0;
  virtual void Close() = 0;
};

class RpcResponder {
 public:
  virtual ~RpcResponder() = default;
  virtual TransferStatus Bind(std::string* endpoint) = 0;
  virtual TransferStatus Receive(std::string* request,
                                 std::unique_ptr<ReplyToken>* token,
                                 Deadline deadline) = 0;
  virtual TransferStatus Reply(std::unique_ptr<ReplyToken> token,
                               const std::string& frame, Deadline deadline) = 0;
  virtual void Close() = 0;
};

// 内部工厂：数据传输的唯一注入点；Registry 仍由 InjectionContext::registry
class IEndpointFactory {
 public:
  virtual ~IEndpointFactory() = default;
  virtual std::unique_ptr<Publisher> CreatePublisher() = 0;
  virtual std::unique_ptr<Subscriber> CreateSubscriber() = 0;
  virtual std::unique_ptr<RpcRequester> CreateRequester() = 0;
  virtual std::unique_ptr<RpcResponder> CreateResponder() = 0;
};

// 注册对象类型：writer(发布者)/reader(订阅者)/service(服务)。
// 定义在此处而非 discovery.h，避免公共头经 interfaces.h 传递包含 Registry。
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

class IRegistry {
 public:
  virtual ~IRegistry() = default;
  virtual bool Register(RegistryKind kind, const std::string& name,
                        const std::string& url) = 0;
  virtual bool Unregister(RegistryKind kind, const std::string& name,
                          const std::string& url) = 0;
  virtual std::vector<std::string> List(RegistryKind kind,
                                        const std::string& name) = 0;
  virtual bool Has(RegistryKind kind, const std::string& name) = 0;
  virtual int Subscribe(RegistryKind kind, const std::string& name,
                        std::function<void(const RegistryEvent&)>&& cb) = 0;
  virtual void Unsubscribe(int id) = 0;
};

// 注入载体。任一字段为空时对应实现退化为默认实现。
struct InjectionContext {
  // 第 2 批注入点；为空时退化为 DefaultEndpointFactory()。
  std::shared_ptr<IEndpointFactory> endpoints;
  std::shared_ptr<IRegistry> registry;
};

std::shared_ptr<IEndpointFactory> DefaultEndpointFactory();
std::shared_ptr<IRegistry> DefaultRegistry();
std::shared_ptr<InjectionContext> DefaultInjection();

}  // namespace detail
}  // namespace firefly

#endif  // FIREFLY_DETAIL_INTERFACES_H_
