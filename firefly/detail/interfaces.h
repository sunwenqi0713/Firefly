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

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace firefly {
namespace detail {

enum class TransportKind { kPub, kSub, kReq, kRep };

// I/O 返回码：0=成功；负值由接口统一定义，避免 nng 错误码外泄。
constexpr int kIoOk = 0;
constexpr int kIoTimeout = -1001;
constexpr int kIoClosed = -1002;
constexpr int kIoQueueFull = -1003;

class ISocket {
 public:
  virtual ~ISocket() = default;

  virtual TransportKind Kind() const = 0;
  // 绑定临时端口，成功写回 url（tcp://...）；非 0 为传输层错误码。
  virtual int BindEphemeral(std::string* url) = 0;
  virtual int Dial(const std::string& url) = 0;
  virtual int Send(const std::string& bytes) = 0;
  // 成功返回 kIoOk 并写回 bytes；超时 kIoTimeout；已关闭 kIoClosed。
  virtual int Recv(std::string* bytes, int timeout_ms) = 0;
  virtual int SetRecvTimeout(int ms) = 0;
  virtual int SetSendTimeout(int ms) = 0;
  virtual bool Valid() const = 0;
  virtual void Close() = 0;
};

class ITransportFactory {
 public:
  virtual ~ITransportFactory() = default;
  virtual std::unique_ptr<ISocket> Create(TransportKind kind) = 0;
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
  std::shared_ptr<ITransportFactory> transport;
  std::shared_ptr<IRegistry> registry;
};

std::shared_ptr<ITransportFactory> DefaultTransportFactory();
std::shared_ptr<IRegistry> DefaultRegistry();
std::shared_ptr<InjectionContext> DefaultInjection();

}  // namespace detail
}  // namespace firefly

#endif  // FIREFLY_DETAIL_INTERFACES_H_
