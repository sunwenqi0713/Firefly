#ifndef FIREFLY_NODE_NODE_H_
#define FIREFLY_NODE_NODE_H_

#include <map>
#include <memory>
#include <mutex>
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

// 创建节点（节点是通信的载体：读写者、服务/客户端都在节点上创建）
std::unique_ptr<Node> CreateNode(const std::string& node_name,
                                 const std::string& name_space = "");

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
 * 名称唯一性契约（本阶段实际行为）：仅 Reader 执行"同节点同通道唯一"检查，
 * 冲突返回 kDuplicate；Writer/Service/Client 不查重（阶段一 out-of-scope），
 * 不会因重名返回错误。
 *
 * 四个 Create 工厂统一返回 Result：
 *   - 成功：r.value 非空，r.code == kOk；
 *   - 失败：r.value 为 nullptr，r.code 指明原因，r.message 必含定位线索；
 *   - 调用方应使用 if (r) / if (!r) 判断，不要直接比较 code。
 */
class Node {
 public:
  friend std::unique_ptr<Node> CreateNode(const std::string&, const std::string&);

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

  // 删除订阅者（同一节点重复删除会失败）
  bool DeleteReader(const std::string& channel_name);

  // 获取已创建的 Reader，不存在或类型不符返回 nullptr
  template <typename MessageT>
  std::shared_ptr<Reader<MessageT>> GetReader(const std::string& channel_name);

  // 把节点下所有 Reader 的新消息拉取到已观察缓存
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
  Node(const std::string& node_name, const std::string& name_space = "");

  std::string node_name_;
  std::string name_space_;

  std::mutex readers_mutex_;
  std::map<std::string, std::shared_ptr<ReaderBase>> readers_;

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
  auto writer = std::make_shared<Writer<MessageT>>(config);
  if (!writer->Init()) {
    const ErrorCode ec = writer->InitErrorCode();
    return R{nullptr, ec,
             detail::CreateFailMessage("CreateWriter", config.channel_name, ec,
                                       writer->InitErrorMessage())};
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
  if (config.pending_queue_size == 0) {
    return R{nullptr, ErrorCode::kInvalidConfig,
             "CreateReader[channel='" + config.channel_name +
                 "']: invalid QoS (pending_queue_size must be > 0)"};
  }

  std::lock_guard<std::mutex> lg(readers_mutex_);
  if (readers_.find(config.channel_name) != readers_.end()) {
    return R{nullptr, ErrorCode::kDuplicate,
             "CreateReader[channel='" + config.channel_name + "', node='" +
                 node_name_ + "']: a Reader already exists on this channel"};
  }
  auto reader = std::make_shared<Reader<MessageT>>(config, reader_func);
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
      node_name_, service_name, service_callback);
  if (!service->Init()) {
    const ErrorCode ec = service->InitErrorCode();
    return R{nullptr, ec,
             detail::CreateFailMessage("CreateService", service_name, ec,
                                       service->InitErrorMessage())};
  }
  return R{service, ErrorCode::kOk, ""};
}

template <typename Request, typename Response>
Result<std::shared_ptr<Client<Request, Response>>> Node::CreateClient(
    const std::string& service_name) {
  using R = Result<std::shared_ptr<Client<Request, Response>>>;
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
      std::make_shared<Client<Request, Response>>(node_name_, service_name);
  if (!client->Init()) {
    const ErrorCode ec = client->InitErrorCode();
    return R{nullptr, ec,
             detail::CreateFailMessage("CreateClient", service_name, ec,
                                       client->InitErrorMessage())};
  }
  return R{client, ErrorCode::kOk, ""};
}

}  // namespace firefly

#endif  // FIREFLY_NODE_NODE_H_
