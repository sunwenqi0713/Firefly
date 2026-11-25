#ifndef FIREFLY_NODE_NODE_H_
#define FIREFLY_NODE_NODE_H_

/// @file node.h
/// @brief 节点 Node 及其端点工厂的声明。
///
/// 节点是通信的载体：Writer、Reader、Service、Client 都在节点上创建。
/// 所有权与查重范围见 Node 类注释；整体契约见 docs/architecture/contracts.md。

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
/// @brief 测试注入点定义所在位置；业务代码不应直接使用。
struct InjectionContext;
}

/**
 * @brief 创建一个节点。
 *
 * @param[in] node_name  节点名；须非空、不超过 255 字节、不含控制字符或空格，
 *                       且在**当前进程内**唯一。
 * @param[in] name_space 命名空间，仅用于日志与属性记录，不参与查重。
 * @return 创建结果。
 * @retval kOk          成功，value 为节点所有权（unique_ptr）。
 * @retval kInvalidName 节点名为空、过长或含非法字符。
 * @retval kDuplicate   当前进程内已存在同名且存活的节点。
 *
 * @sa CreateNode(const std::string&, const std::string&,
 *                const std::shared_ptr<detail::InjectionContext>&)
 */
Result<std::unique_ptr<Node>> CreateNode(const std::string& node_name,
                                         const std::string& name_space = "");

/**
 * @brief 创建一个带注入点的节点，供测试替换默认后端与注册中心。
 *
 * @param[in] node_name  节点名，约束同上。
 * @param[in] name_space 命名空间，约束同上。
 * @param[in] ctx        注入点；为空时所有端点使用默认后端与默认注册中心，
 *                       行为与另一个重载完全一致。
 * @return 创建结果，返回码含义同上。
 * @note 端点的所有权不随节点转移：注入只决定端点由哪个后端实现，节点析构
 *       不会销毁端点。
 */
Result<std::unique_ptr<Node>> CreateNode(
    const std::string& node_name, const std::string& name_space,
    const std::shared_ptr<detail::InjectionContext>& ctx);

namespace detail {

// ---------------------------------------------------------------------------
// 名称与配置校验
// ---------------------------------------------------------------------------

/// @brief 判断端点名是否合法。
/// @param[in] name 待校验名称。
/// @return 非空、不超过 255 字节且不含控制字符与空格时返回 true。
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

/// @brief 判断字符串是否以指定后缀结尾。
/// @param[in] s      待检查字符串。
/// @param[in] suffix 后缀；不得为空指针。
/// @return 以 suffix 结尾时返回 true。
inline bool EndsWith(const std::string& s, const char* suffix) {
  const std::string suf(suffix);
  return s.size() >= suf.size() &&
         s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

/// @brief 校验通道名。
/// @param[in] name 通道名。
/// @return 合法返回 kOk；否则返回 kInvalidName。
/// @note 通道名不得命中服务内部通道的保留后缀，否则与服务实现冲突。
inline ErrorCode ValidateChannelName(const std::string& name) {
  if (!IsLegalEndpointName(name) || EndsWith(name, SRV_CHANNEL_REQ_SUFFIX) ||
      EndsWith(name, SRV_CHANNEL_RES_SUFFIX)) {
    return ErrorCode::kInvalidName;
  }
  return ErrorCode::kOk;
}

/// @brief 校验服务名。
/// @param[in] name 服务名。
/// @return 合法返回 kOk；否则返回 kInvalidName。
/// @note 服务名不受通道保留后缀限制。
inline ErrorCode ValidateServiceName(const std::string& name) {
  return IsLegalEndpointName(name) ? ErrorCode::kOk : ErrorCode::kInvalidName;
}

/// @brief 校验节点名。
/// @param[in] name 节点名。
/// @return 合法返回 kOk；否则返回 kInvalidName。
inline ErrorCode ValidateNodeName(const std::string& name) {
  return IsLegalEndpointName(name) ? ErrorCode::kOk : ErrorCode::kInvalidName;
}

/// @brief 校验 QoS 配置。
/// @param[in] qos 待校验配置。
/// @return 合法返回 kOk；depth 为 0 时返回 kInvalidConfig。
inline ErrorCode ValidateQos(const QosProfile& qos) {
  if (qos.depth == 0) {
    return ErrorCode::kInvalidConfig;  // depth==0 无意义，fail-fast
  }
  return ErrorCode::kOk;
}

/// @brief 拼接创建失败时的定位线索，保证任何失败路径的 message 非空。
/// @param[in] op           操作名，如 `"CreateWriter"`。
/// @param[in] key          对象名（通道名或服务名）。
/// @param[in] code         失败原因码。
/// @param[in] impl_message 实现层给出的细节；为空时用占位文案替代。
/// @return 形如 `CreateWriter[channel='x']: <细节> (code=kInvalidName)` 的字符串。
inline std::string CreateFailMessage(const char* op, const std::string& key,
                                     ErrorCode code,
                                     const std::string& impl_message) {
  std::string m = impl_message.empty() ? std::string("Init failed") : impl_message;
  return std::string(op) + "[" + key + "]: " + m +
         " (code=" + ErrorCodeToString(code) + ")";
}

}  // namespace detail

/**
 * @brief 通信的基本单元：一个模块通过 Node 创建读写者与/或服务客户端。
 *
 * @par 名称唯一性
 * - Node：进程内唯一，重名返回 kDuplicate，析构时释放名字；不同进程的同名
 *   节点互不冲突。
 * - Reader、Writer：单个 Node 内同一通道唯一，重复返回 kDuplicate。
 * - Service、Client：单个 Node 内同一服务名唯一，重复返回 kDuplicate。
 * - 跨进程同名不由框架拦截：不同进程的 Writer/Service 可以同名共存，订阅方
 *   与客户端会看到多个端点并任选其一。这属已知缺口，见
 *   docs/architecture/known-gaps.md。
 * - 单 Node 内的查重只比较仍存活的端点；端点被外部释放或被 DeleteXxx 之后，
 *   名字可再次使用。
 *
 * @par 所有权
 * 节点持有它创建的 Reader，并在析构时关闭它们。Writer、Service、Client 由
 * 调用方通过 shared_ptr 持有，节点只保存弱引用用于查重与 DeleteXxx 定位，
 * 析构节点不会关闭它们。
 *
 * @par 创建结果
 * 四个 Create 工厂统一返回 Result：成功时 value 非空、code 为 kOk；失败时
 * value 为空、code 指明原因、message 必含定位线索。调用方应使用 `if (r)` /
 * `if (!r)` 判断，不要直接比较 code。
 *
 * @sa docs/architecture/contracts.md §1 「资源所有权」
 */
class Node {
 public:
  friend Result<std::unique_ptr<Node>> CreateNode(const std::string&,
                                                  const std::string&);
  friend Result<std::unique_ptr<Node>> CreateNode(
      const std::string&, const std::string&,
      const std::shared_ptr<detail::InjectionContext>&);

  /**
   * @brief 析构节点。
   *
   * 关闭本节点创建的全部 Reader，并释放节点名。Writer、Service、Client 不受
   * 影响，仍由其持有者负责关闭。
   */
  virtual ~Node();

  /// @brief 取节点名。
  const std::string& Name() const { return node_name_; }

  /// @brief 取命名空间。
  const std::string& NameSpace() const { return name_space_; }

  // ---------------- 发布 / 订阅 ----------------

  /**
   * @brief 在指定通道上创建发布者，使用默认 QoS。
   *
   * @tparam MessageT 消息类型，须为 google::protobuf::Message 的派生类。
   * @param[in] channel_name 通道名。
   * @return 创建结果，返回码含义见接收 ChannelConfig 的重载。
   */
  template <typename MessageT>
  Result<std::shared_ptr<Writer<MessageT>>> CreateWriter(
      const std::string& channel_name);

  /**
   * @brief 在指定通道上创建发布者，可指定 QoS。
   *
   * @tparam MessageT 消息类型，须为 google::protobuf::Message 的派生类。
   * @param[in] config 通道名与 QoS 配置。
   * @return 创建结果。
   * @retval kOk            成功，value 为发布者。
   * @retval kNotReady      firefly::Init() 未调用，或 Clear() 已执行。
   * @retval kInvalidName   通道名为空、过长、含非法字符，或命中服务通道保留后缀。
   * @retval kInvalidConfig QoS 非法，即 depth 为 0。
   * @retval kDuplicate     本节点内该通道已有存活的 Writer，或另一线程正在创建同名端点。
   * @retval 其它            端点初始化失败时透传底层错误码，message 含细节。
   */
  template <typename MessageT>
  Result<std::shared_ptr<Writer<MessageT>>> CreateWriter(
      const ChannelConfig& config);

  /**
   * @brief 在指定通道上创建订阅者，使用默认配置。
   *
   * @tparam MessageT 消息类型，须为 google::protobuf::Message 的派生类。
   * @param[in] channel_name 通道名。
   * @param[in] reader_func  回调；为 nullptr 时进入 Observe 模式。
   * @return 创建结果，返回码含义见接收 ReaderConfig 的重载。
   */
  template <typename MessageT>
  Result<std::shared_ptr<Reader<MessageT>>> CreateReader(
      const std::string& channel_name,
      const CallbackFunc<MessageT>& reader_func = nullptr);

  /**
   * @brief 在指定通道上创建订阅者，可指定 QoS 与回调。
   *
   * @tparam MessageT 消息类型，须为 google::protobuf::Message 的派生类。
   * @param[in] config      通道名与 QoS 配置；depth 决定观察队列深度。
   * @param[in] reader_func 回调；为 nullptr 时进入 Observe 模式。
   * @return 创建结果。
   * @retval kOk            成功，value 为订阅者。
   * @retval kNotReady      firefly::Init() 未调用，或 Clear() 已执行。
   * @retval kInvalidName   通道名非法。
   * @retval kInvalidConfig QoS 非法，即 depth 为 0。
   * @retval kDuplicate     本节点内该通道已有存活的 Reader，或同名 Reader 正在删除。
   * @retval 其它            端点初始化失败时透传底层错误码，message 含细节。
   */
  template <typename MessageT>
  Result<std::shared_ptr<Reader<MessageT>>> CreateReader(
      const ReaderConfig& config,
      const CallbackFunc<MessageT>& reader_func = nullptr);

  /**
   * @brief 删除本节点创建并持有的订阅者。
   *
   * 删除过程分两段：先在锁内标记「正在删除」但保留表项，释放节点锁后才执行
   * Reader::Shutdown()（join 收线程并等待在途回调），完成后移除表项与标记。
   * 因此在回调内并发调用 GetReader() 不会死锁，且仍能拿到旧的 Reader。
   *
   * @param[in] channel_name 通道名。
   * @return 成功关闭并移除返回 true；不存在或已在删除中返回 false。
   * @post 返回 true 后该通道名可重新创建 Reader。
   */
  bool DeleteReader(const std::string& channel_name);

  /**
   * @brief 删除本节点注册的发布者，并对其调用 Shutdown()。
   *
   * @param[in] channel_name 通道名。
   * @return 成功为 kOk；名字不存在或端点已被外部释放时为 kNotReady。
   * @post 名字可重新用于创建 Writer。
   */
  Result<void> DeleteWriter(const std::string& channel_name);

  /**
   * @brief 删除本节点注册的服务，并对其调用 Shutdown()。
   *
   * @param[in] service_name 服务名。
   * @return 成功为 kOk；名字不存在或端点已被外部释放时为 kNotReady。
   * @post 名字可重新用于创建 Service。
   */
  Result<void> DeleteService(const std::string& service_name);

  /**
   * @brief 删除本节点注册的客户端，并对其调用 Shutdown()。
   *
   * @param[in] service_name 服务名。
   * @return 成功为 kOk；名字不存在或端点已被外部释放时为 kNotReady。
   * @post 名字可重新用于创建 Client。
   */
  Result<void> DeleteClient(const std::string& service_name);

  /**
   * @brief 取本节点创建的订阅者。
   *
   * @tparam MessageT 期望的消息类型。
   * @param[in] channel_name 通道名。
   * @return 订阅者指针；不存在或实际类型与 MessageT 不符时返回 nullptr。
   * @par 线程安全
   * 可与其他成员函数并发调用，也可在 Reader 回调内调用。
   */
  template <typename MessageT>
  std::shared_ptr<Reader<MessageT>> GetReader(const std::string& channel_name);

  /**
   * @brief 对全部 Reader 触发一次拉取。
   *
   * @note 保留以兼容既有调用方与 ReaderBase 接口：本实现中消息在收线程直接
   *       入队，Reader::Observe() 是空实现，因此本方法当前不产生额外效果。
   * @par 线程安全
   * Reader 表由内部互斥量保护，可并发调用。
   */
  void Observe();

  /// @brief 清空本节点全部 Reader 的观察缓存。
  /// @par 线程安全
  /// Reader 表由内部互斥量保护，可并发调用。
  void ClearData();

  // ---------------- 服务 / 客户端 ----------------

  /**
   * @brief 创建服务。
   *
   * @tparam Request  请求消息类型，须为 google::protobuf::Message 的派生类。
   * @tparam Response 响应消息类型，同上。
   * @param[in] service_name     服务名。
   * @param[in] service_callback 业务回调，不得为空。
   * @return 创建结果。
   * @retval kOk           成功，value 为服务。
   * @retval kNotReady     firefly::Init() 未调用，或 Clear() 已执行。
   * @retval kInvalidName  服务名为空、过长或含非法字符。
   * @retval kDuplicate    本节点内已有同名且存活的 Service，或另一线程正在创建同名端点。
   * @retval 其它           端点初始化失败时透传底层错误码，message 含细节。
   */
  template <typename Request, typename Response>
  Result<std::shared_ptr<Service<Request, Response>>> CreateService(
      const std::string& service_name,
      const typename Service<Request, Response>::ServiceCallback& service_callback);

  /**
   * @brief 创建指向指定服务的客户端。
   *
   * @tparam Request  请求消息类型，须为 google::protobuf::Message 的派生类。
   * @tparam Response 响应消息类型，同上。
   * @param[in] service_name 目标服务名。
   * @return 创建结果。
   * @retval kOk           成功，value 为客户端。
   * @retval kNotReady     firefly::Init() 未调用，或 Clear() 已执行。
   * @retval kInvalidName  服务名非法。
   * @retval kDuplicate    本节点内已有同名且存活的 Client，或另一线程正在创建同名端点。
   * @retval 其它           端点初始化失败时透传底层错误码，message 含细节。
   */
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

  // Writer/Service/Client 的弱引用注册表，仅用于单 Node 查重与 DeleteXxx 定位。
  // 弱引用不改变「端点由外部 shared_ptr 持有、独立存活」的所有权：节点析构
  // 不关闭这些端点。
  // key 编码为 "W:"+channel / "S:"+service / "C:"+service（类型隔离命名空间）。
  mutable std::mutex endpoints_mutex_;
  std::map<std::string, std::weak_ptr<WriterBase>> writers_;
  std::map<std::string, std::weak_ptr<ServiceBase>> services_;
  std::map<std::string, std::weak_ptr<ClientBase>> clients_;

  // 进行中的创建占位，受 endpoints_mutex_ 保护。查重与占位必须在同一把锁内
  // 完成：否则「查重通过 → 初始化 → 落表」这段窗口里，多个线程会各自通过
  // 查重、各自创建成功，弱引用表最终只留下最后一个，DeleteXxx 再也定位不到
  // 其余实例，同名查重也就永久失效了。
  std::set<std::string> endpoints_creating_;

  /// @brief 查重并占位；返回 false 表示已有存活端点或另一线程正在创建同名端点。
  template <typename Map>
  bool ReserveEndpoint(const std::string& key, const Map& endpoints) {
    std::lock_guard<std::mutex> lg(endpoints_mutex_);
    if (endpoints_creating_.count(key) != 0) return false;
    auto it = endpoints.find(key);
    if (it != endpoints.end() && !it->second.expired()) return false;
    endpoints_creating_.insert(key);
    return true;
  }

  /// @brief 释放占位（创建失败或提前返回）。
  void AbandonEndpoint(const std::string& key) {
    std::lock_guard<std::mutex> lg(endpoints_mutex_);
    endpoints_creating_.erase(key);
  }

  /// @brief 创建成功：落表并释放占位。同一把锁内完成，不存在「无占位也未落表」的可见窗口。
  template <typename Map, typename Endpoint>
  void PublishEndpoint(const std::string& key, Map* endpoints,
                       const std::shared_ptr<Endpoint>& endpoint) {
    std::lock_guard<std::mutex> lg(endpoints_mutex_);
    (*endpoints)[key] = endpoint;
    endpoints_creating_.erase(key);
  }

  /// @brief 占位的作用域守卫：未 Disarm() 时析构即释放占位。
  class Reservation {
   public:
    Reservation(Node* owner, std::string key)
        : owner_(owner), key_(std::move(key)) {}
    ~Reservation() {
      if (active_) owner_->AbandonEndpoint(key_);
    }
    Reservation(const Reservation&) = delete;
    Reservation& operator=(const Reservation&) = delete;
    /// @brief 落表成功后解除守卫，避免重复释放（可能误删后来者的占位）。
    void Disarm() { active_ = false; }

   private:
    Node* owner_;
    std::string key_;
    bool active_ = true;
  };

  // 内部实现：拓扑注册与服务发现等元信息。
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
  if (!ReserveEndpoint(key, writers_)) {
    return R{nullptr, ErrorCode::kDuplicate,
             "CreateWriter[channel='" + config.channel_name + "', node='" +
                 node_name_ + "']: a Writer already exists on this channel"};
  }
  Reservation reservation(this, key);
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
  PublishEndpoint(key, &writers_, writer);
  reservation.Disarm();
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
  if (!ReserveEndpoint(key, services_)) {
    return R{nullptr, ErrorCode::kDuplicate,
             "CreateService[service='" + service_name + "', node='" +
                 node_name_ + "']: a Service already exists with this name"};
  }
  Reservation reservation(this, key);
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
  PublishEndpoint(key, &services_, service);
  reservation.Disarm();
  return R{service, ErrorCode::kOk, ""};
}

template <typename Request, typename Response>
Result<std::shared_ptr<Client<Request, Response>>> Node::CreateClient(
    const std::string& service_name) {
  using R = Result<std::shared_ptr<Client<Request, Response>>>;
  const std::string key = "C:" + service_name;
  if (!ReserveEndpoint(key, clients_)) {
    return R{nullptr, ErrorCode::kDuplicate,
             "CreateClient[service='" + service_name + "', node='" +
                 node_name_ + "']: a Client already exists for this service"};
  }
  Reservation reservation(this, key);
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
  PublishEndpoint(key, &clients_, client);
  reservation.Disarm();
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
