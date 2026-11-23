#ifndef FIREFLY_NODE_NODE_H_
#define FIREFLY_NODE_NODE_H_

#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>

#include "firefly/common/types.h"
#include "firefly/init.h"
#include "firefly/node/channel_config.h"
#include "firefly/node/reader.h"
#include "firefly/node/reader_base.h"
#include "firefly/node/writer.h"
#include "firefly/service/client.h"
#include "firefly/service/service.h"

namespace firefly {

class Node;

namespace detail {
struct InjectionContext;  // 定义于 src/core/injection.h（注入点，阶段三）
}

// 创建节点（节点是通信的载体：读写者、服务/客户端都在节点上创建）。
// 阶段二：返回 Result —— 进程内节点名重复时返回 ErrorCode::kDuplicate。
// 合法性（空名/非法字符）返回 kInvalidName；成功时 value 非空。
Result<std::unique_ptr<Node>> CreateNode(const std::string& node_name,
                                         const std::string& name_space = "");

// 阶段三：带注入点的工厂重载。fake 注入发生在 CreateXxx 工厂层；端点仍由
// 外部 shared_ptr 独立持有（阶段二方案 b），Node 生命周期不控制端点存亡。
// ctx 为空时所有端点退化为默认后端 + 默认 Registry
// （FIREFLY_WITH_NNG=ON 为 NNG，OFF 为内存），行为完全不变。
Result<std::unique_ptr<Node>> CreateNode(
    const std::string& node_name, const std::string& name_space,
    const std::shared_ptr<detail::InjectionContext>& ctx);

namespace detail {

// ---------------------------------------------------------------------------
// 名称与配置校验（阶段一：Create 前置校验，失败返回 kInvalidName/kInvalidConfig）
// ---------------------------------------------------------------------------
inline bool IsLegalEndpointName(const std::string& name) {
  if (name.empty() || name.size() > 255) {
    return false;
  }
  for (unsigned char c : name) {
    if (c <= 0x20 || c == 0x7f) {  // 控制字符与空格一律非法
      return false;
    }
  }
  return true;
}

inline bool EndsWith(const std::string& s, const char* suffix) {
  const std::string suf(suffix);
  return s.size() >= suf.size() &&
         s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

// 通道名不得命中服务内部通道的后缀保留字
inline ErrorCode ValidateChannelName(const std::string& name) {
  if (!IsLegalEndpointName(name) || EndsWith(name, SRV_CHANNEL_REQ_SUFFIX) ||
      EndsWith(name, SRV_CHANNEL_RES_SUFFIX)) {
    return ErrorCode::kInvalidName;
  }
  return ErrorCode::kOk;
}

inline ErrorCode ValidateServiceName(const std::string& name) {
  return IsLegalEndpointName(name) ? ErrorCode::kOk : ErrorCode::kInvalidName;
}

// 节点名校验：阶段二 CreateNode 进程内查重前置校验（与服务名同一套合法性）
inline ErrorCode ValidateNodeName(const std::string& name) {
  return IsLegalEndpointName(name) ? ErrorCode::kOk : ErrorCode::kInvalidName;
}

inline ErrorCode ValidateQos(const QosProfile& qos) {
  if (qos.depth == 0) {
    return ErrorCode::kInvalidConfig;  // depth==0 无意义，fail-fast
  }
  return ErrorCode::kOk;
}

// Impl 层未填 message 时兜底，保证任何失败路径 message 非空。
inline std::string CreateFailMessage(const char* op, const std::string& key,
                                     ErrorCode code,
                                     const std::string& impl_message) {
  std::string m = impl_message.empty() ? std::string("Init failed") : impl_message;
  return std::string(op) + "[" + key + "]: " + m +
         " (code=" + ErrorCodeToString(code) + ")";
}

}  // namespace detail

/**
 * @class Node
 * @brief 通信的基本单元。一个模块通过 Node 定义读写者与/或服务客户端。
 *
 * 名称唯一性契约（阶段二起，精确描述每类对象的查重范围）：
 *   - Node      ：进程内唯一（复用 ShutdownManager 注册表），重复返回 kDuplicate；
 *                 节点析构时释放名字。不同进程间同名节点互不冲突。
 *   - Reader    ：单 Node 内同一通道唯一，重复返回 kDuplicate（阶段一已有）。
 *   - Writer    ：单 Node 内同一通道唯一（按 channel_name），重复返回 kDuplicate。
 *   - Service   ：单 Node 内服务名唯一，重复返回 kDuplicate。
 *   - Client    ：单 Node 内同一服务名唯一，重复返回 kDuplicate。
 *   跨进程同名：发现协议不处理名字冲突；不同进程的 Writer/Service 可以同名
 *   静默共存（订阅方/客户端会看到多个端点，选择其中一个连接）。这是已知缺口，
 *   需要发现协议扩展才能拦截，本阶段不修。
 *   "单 Node 内查重"仅比较仍存活（weak_ptr 未过期）的端点；端点被外部释放或
 *   DeleteXxx 后名字可回收再建。
 *
 * 四个 Create 工厂统一返回 Result：
 *   - 成功：r.value 非空，r.code == kOk；
 *   - 失败：r.value 为 nullptr，r.code 指明原因，r.message 必含定位线索；
 *   - 调用方应使用 if (r) / if (!r) 判断，不要直接比较 code。
 */
class Node {
 public:
  friend Result<std::unique_ptr<Node>> CreateNode(const std::string&,
                                                  const std::string&);
  friend Result<std::unique_ptr<Node>> CreateNode(
      const std::string&, const std::string&,
      const std::shared_ptr<detail::InjectionContext>&);

  // 定义于 node.cc（实现阶段补充），以保证唯一指针 impl_ 在完整类型处析构
  virtual ~Node();

  const std::string& Name() const { return node_name_; }
  const std::string& NameSpace() const { return name_space_; }

  // ---------------- 发布 / 订阅 ----------------

  // 创建发布者（按通道名，使用默认 QoS）
  template <typename MessageT>
  Result<std::shared_ptr<Writer<MessageT>>> CreateWriter(
      const std::string& channel_name);

  // 创建发布者（按通道配置，可指定 QoS）
  template <typename MessageT>
  Result<std::shared_ptr<Writer<MessageT>>> CreateWriter(
      const ChannelConfig& config);

  // 创建订阅者（按通道名，使用默认配置；可传回调）
  template <typename MessageT>
  Result<std::shared_ptr<Reader<MessageT>>> CreateReader(
      const std::string& channel_name,
      const CallbackFunc<MessageT>& reader_func = nullptr);

  // 创建订阅者（按 ReaderConfig 配置）
  template <typename MessageT>
  Result<std::shared_ptr<Reader<MessageT>>> CreateReader(
      const ReaderConfig& config,
      const CallbackFunc<MessageT>& reader_func = nullptr);

  // 删除订阅者（不存在返回 false）
  // 删除中契约（第 1 批）：锁内在 readers_deleting_ 标记"正在删除"，但暂不
  // 释放 readers_ 表项；释放节点锁后才调用 Reader::Shutdown()（会 join 收
  // 线程并等待在途回调，回调内可并发调用 GetReader/Observe），完成后才在
  // 锁内移除标记与表项。因此：回调内 GetReader 不会死锁且仍拿到旧 Reader；
  // 删除中的同名 CreateReader 返回 kDuplicate；Shutdown 返回后可重建同名。
  bool DeleteReader(const std::string& channel_name);

  // 删除发布者 / 服务 / 客户端（阶段二，返回 Result<void>）：
  //   成功 → kOk 并对该端点调用 Shutdown；不存在（或外部已释放）→ kNotReady。
  // 删除后 Node 不再持有该名字；同名可重新创建。
  Result<void> DeleteWriter(const std::string& channel_name);
  Result<void> DeleteService(const std::string& service_name);
  Result<void> DeleteClient(const std::string& service_name);

  // 获取已创建的 Reader，不存在或类型不符返回 nullptr
  template <typename MessageT>
  std::shared_ptr<Reader<MessageT>> GetReader(const std::string& channel_name);

  // 把节点下所有 Reader 的新消息拉取到已观察缓存
  // @threadsafe readers_ 由 readers_mutex_ 保护；逐个调用 Reader::Observe()
  // （收线程内已由 queue_mutex_ 保护），可并发调用。
  void Observe();
  // 清空节点下所有 Reader 的缓存数据
  void ClearData();

  // ---------------- 服务 / 客户端 ----------------

  // 创建服务（service_callback 处理请求并填充响应）
  template <typename Request, typename Response>
  Result<std::shared_ptr<Service<Request, Response>>> CreateService(
      const std::string& service_name,
      const typename Service<Request, Response>::ServiceCallback& service_callback);

  // 创建指向指定服务的客户端
  template <typename Request, typename Response>
  Result<std::shared_ptr<Client<Request, Response>>> CreateClient(
      const std::string& service_name);

 private:
  Node(const std::string& node_name, const std::string& name_space = "",
       const std::shared_ptr<detail::InjectionContext>& ctx = nullptr);

  std::string node_name_;
  std::string name_space_;
  std::shared_ptr<detail::InjectionContext> ctx_;

  std::mutex readers_mutex_;
  std::map<std::string, std::shared_ptr<ReaderBase>> readers_;
  // 正在 Shutdown 的 Reader 名称；表项保留到 Shutdown 完成，防止删除中
  // 重建同名端点或旧端点注销误伤新端点。
  std::set<std::string> readers_deleting_;

  // 阶段二：Writer/Service/Client 的弱引用注册表，仅用于单 Node 查重与
  // DeleteXxx 定位。弱引用不改变"端点由外部 shared_ptr 持有、独立存活"的
  // 所有权现状；析构时如需关闭端点遵循下面的 Node 契约。
  // key 编码为 "W:"+channel / "S:"+service / "C:"+service（类型隔离命名空间）。
  mutable std::mutex endpoints_mutex_;
  std::map<std::string, std::weak_ptr<WriterBase>> writers_;
  std::map<std::string, std::weak_ptr<ServiceBase>> services_;
  std::map<std::string, std::weak_ptr<ClientBase>> clients_;

  // 内部实现（拓扑注册、服务发现等），实现阶段补充
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

template <typename MessageT>
Result<std::shared_ptr<Writer<MessageT>>> Node::CreateWriter(
    const std::string& channel_name) {
  ChannelConfig config;
  config.channel_name = channel_name;
  return CreateWriter<MessageT>(config);
}

template <typename MessageT>
Result<std::shared_ptr<Writer<MessageT>>> Node::CreateWriter(
    const ChannelConfig& config) {
  using R = Result<std::shared_ptr<Writer<MessageT>>>;
  const std::string key = "W:" + config.channel_name;
  {
    std::lock_guard<std::mutex> lg(endpoints_mutex_);
    auto it = writers_.find(key);
    if (it != writers_.end() && !it->second.expired()) {
      return R{nullptr, ErrorCode::kDuplicate,
               "CreateWriter[channel='" + config.channel_name + "', node='" +
                   node_name_ + "']: a Writer already exists on this channel"};
    }
  }
  if (!OK()) {
    return R{nullptr, ErrorCode::kNotReady,
             "CreateWriter: firefly::Init() has not been called (or Clear() "
             "already ran)"};
  }
  if (ErrorCode ec = detail::ValidateChannelName(config.channel_name);
      ec != ErrorCode::kOk) {
    return R{nullptr, ec,
             "CreateWriter[channel='" + config.channel_name +
                 "']: invalid channel name (empty/illegal char/reserved "
                 "__SRV__ suffix)"};
  }
  if (ErrorCode ec = detail::ValidateQos(config.qos_profile);
      ec != ErrorCode::kOk) {
    return R{nullptr, ec,
             "CreateWriter[channel='" + config.channel_name +
                 "']: invalid QoS (depth must be > 0)"};
  }
  auto writer = std::make_shared<Writer<MessageT>>(config, ctx_);
  if (!writer->Init()) {
    const ErrorCode ec = writer->InitErrorCode();
    return R{nullptr, ec,
             detail::CreateFailMessage("CreateWriter", config.channel_name, ec,
                                       writer->InitErrorMessage())};
  }
  {
    std::lock_guard<std::mutex> lg(endpoints_mutex_);
    writers_[key] = writer;
  }
  return R{writer, ErrorCode::kOk, ""};
}

template <typename MessageT>
Result<std::shared_ptr<Reader<MessageT>>> Node::CreateReader(
    const std::string& channel_name,
    const CallbackFunc<MessageT>& reader_func) {
  ReaderConfig config;
  config.channel_name = channel_name;
  return CreateReader<MessageT>(config, reader_func);
}

template <typename MessageT>
Result<std::shared_ptr<Reader<MessageT>>> Node::CreateReader(
    const ReaderConfig& config, const CallbackFunc<MessageT>& reader_func) {
  using R = Result<std::shared_ptr<Reader<MessageT>>>;
  if (!OK()) {
    return R{nullptr, ErrorCode::kNotReady,
             "CreateReader: firefly::Init() has not been called (or Clear() "
             "already ran)"};
  }
  if (ErrorCode ec = detail::ValidateChannelName(config.channel_name);
      ec != ErrorCode::kOk) {
    return R{nullptr, ec,
             "CreateReader[channel='" + config.channel_name +
                 "']: invalid channel name (empty/illegal char/reserved "
                 "__SRV__ suffix)"};
  }
  if (ErrorCode ec = detail::ValidateQos(config.qos_profile);
      ec != ErrorCode::kOk) {
    return R{nullptr, ec,
             "CreateReader[channel='" + config.channel_name +
                 "']: invalid QoS (depth must be > 0)"};
  }
  std::lock_guard<std::mutex> lg(readers_mutex_);
  if (readers_.find(config.channel_name) != readers_.end() ||
      readers_deleting_.count(config.channel_name) != 0) {
    return R{nullptr, ErrorCode::kDuplicate,
             "CreateReader[channel='" + config.channel_name + "', node='" +
                 node_name_ + "']: a Reader already exists (or is being deleted) on this channel"};
  }
  auto reader = std::make_shared<Reader<MessageT>>(config, reader_func, ctx_);
  if (!reader->Init()) {
    const ErrorCode ec = reader->InitErrorCode();
    return R{nullptr, ec,
             detail::CreateFailMessage("CreateReader", config.channel_name, ec,
                                       reader->InitErrorMessage())};
  }
  readers_.emplace(config.channel_name, reader);
  return R{reader, ErrorCode::kOk, ""};
}

template <typename MessageT>
std::shared_ptr<Reader<MessageT>> Node::GetReader(
    const std::string& channel_name) {
  std::lock_guard<std::mutex> lg(readers_mutex_);
  auto it = readers_.find(channel_name);
  if (it != readers_.end()) {
    return std::dynamic_pointer_cast<Reader<MessageT>>(it->second);
  }
  return nullptr;
}

template <typename Request, typename Response>
Result<std::shared_ptr<Service<Request, Response>>> Node::CreateService(
    const std::string& service_name,
    const typename Service<Request, Response>::ServiceCallback& service_callback) {
  using R = Result<std::shared_ptr<Service<Request, Response>>>;
  const std::string key = "S:" + service_name;
  {
    std::lock_guard<std::mutex> lg(endpoints_mutex_);
    auto it = services_.find(key);
    if (it != services_.end() && !it->second.expired()) {
      return R{nullptr, ErrorCode::kDuplicate,
               "CreateService[service='" + service_name + "', node='" +
                   node_name_ + "']: a Service already exists with this name"};
    }
  }
  if (!OK()) {
    return R{nullptr, ErrorCode::kNotReady,
             "CreateService: firefly::Init() has not been called (or Clear() "
             "already ran)"};
  }
  if (ErrorCode ec = detail::ValidateServiceName(service_name);
      ec != ErrorCode::kOk) {
    return R{nullptr, ec,
             "CreateService[service='" + service_name +
                 "']: invalid service name"};
  }
  auto service = std::make_shared<Service<Request, Response>>(
      node_name_, service_name, service_callback, ctx_);
  if (!service->Init()) {
    const ErrorCode ec = service->InitErrorCode();
    return R{nullptr, ec,
             detail::CreateFailMessage("CreateService", service_name, ec,
                                       service->InitErrorMessage())};
  }
  {
    std::lock_guard<std::mutex> lg(endpoints_mutex_);
    services_[key] = service;
  }
  return R{service, ErrorCode::kOk, ""};
}

template <typename Request, typename Response>
Result<std::shared_ptr<Client<Request, Response>>> Node::CreateClient(
    const std::string& service_name) {
  using R = Result<std::shared_ptr<Client<Request, Response>>>;
  const std::string key = "C:" + service_name;
  {
    std::lock_guard<std::mutex> lg(endpoints_mutex_);
    auto it = clients_.find(key);
    if (it != clients_.end() && !it->second.expired()) {
      return R{nullptr, ErrorCode::kDuplicate,
               "CreateClient[service='" + service_name + "', node='" +
                   node_name_ + "']: a Client already exists for this service"};
    }
  }
  if (!OK()) {
    return R{nullptr, ErrorCode::kNotReady,
             "CreateClient: firefly::Init() has not been called (or Clear() "
             "already ran)"};
  }
  if (ErrorCode ec = detail::ValidateServiceName(service_name);
      ec != ErrorCode::kOk) {
    return R{nullptr, ec,
             "CreateClient[service='" + service_name +
                 "']: invalid service name"};
  }
  auto client =
      std::make_shared<Client<Request, Response>>(node_name_, service_name, ctx_);
  if (!client->Init()) {
    const ErrorCode ec = client->InitErrorCode();
    return R{nullptr, ec,
             detail::CreateFailMessage("CreateClient", service_name, ec,
                                       client->InitErrorMessage())};
  }
  {
    std::lock_guard<std::mutex> lg(endpoints_mutex_);
    clients_[key] = client;
  }
  return R{client, ErrorCode::kOk, ""};
}

// DeleteXxx 实现（按名字定位弱引用，成功后 Shutdown 并移除表项）
inline Result<void> Node::DeleteService(const std::string& service_name) {
  const std::string key = "S:" + service_name;
  std::shared_ptr<ServiceBase> sp;
  {
    std::lock_guard<std::mutex> lg(endpoints_mutex_);
    auto it = services_.find(key);
    if (it == services_.end()) {
      return {ErrorCode::kNotReady,
              "DeleteService[service='" + service_name + "']: not found"};
    }
    sp = it->second.lock();
    services_.erase(it);
  }
  if (sp) sp->Shutdown();
  return {ErrorCode::kOk, ""};
}

inline Result<void> Node::DeleteClient(const std::string& service_name) {
  const std::string key = "C:" + service_name;
  std::shared_ptr<ClientBase> sp;
  {
    std::lock_guard<std::mutex> lg(endpoints_mutex_);
    auto it = clients_.find(key);
    if (it == clients_.end()) {
      return {ErrorCode::kNotReady,
              "DeleteClient[service='" + service_name + "']: not found"};
    }
    sp = it->second.lock();
    clients_.erase(it);
  }
  if (sp) sp->Shutdown();
  return {ErrorCode::kOk, ""};
}

inline Result<void> Node::DeleteWriter(const std::string& channel_name) {
  const std::string key = "W:" + channel_name;
  std::shared_ptr<WriterBase> sp;
  {
    std::lock_guard<std::mutex> lg(endpoints_mutex_);
    auto it = writers_.find(key);
    if (it == writers_.end()) {
      return {ErrorCode::kNotReady,
              "DeleteWriter[channel='" + channel_name + "']: not found"};
    }
    sp = it->second.lock();
    writers_.erase(it);
  }
  if (sp) sp->Shutdown();
  return {ErrorCode::kOk, ""};
}

}  // namespace firefly

#endif  // FIREFLY_NODE_NODE_H_
