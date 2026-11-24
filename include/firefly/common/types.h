#ifndef FIREFLY_COMMON_TYPES_H_
#define FIREFLY_COMMON_TYPES_H_

/// @file types.h
/// @brief 错误码、结果类型、回调上下文、QoS 配置与端点统计等公共值类型。

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace firefly {

/**
 * @brief 无消息体的占位类型，用于仅需一次调用的消息定义。
 */
class NullType {};

// ---------------------------------------------------------------------------
// 错误码与结果类型
// ---------------------------------------------------------------------------

/**
 * @brief 旧的二值错误码。
 *
 * @deprecated 自 0.5 起废弃，改用 ErrorCode 配合 Result<T>；两者不得混用。
 */
enum [[deprecated("Firefly: use ErrorCode with Result<T> instead")]] ReturnCode {
  SUCC = 0,  ///< 成功，等价于 ErrorCode::kOk。
  FAIL = 1   ///< 失败；无法区分原因，不要在新代码中使用。
};

/**
 * @brief 端点操作与通信过程的统一错误码。
 *
 * 每个码都对应一条可触发的失败路径；禁止用 bool 或空指针折叠失败原因。
 * 错误码可随 RPC 响应帧跨进程传递，唯 kCancelled 只在本地兑现、不上线。
 * @sa docs/architecture/contracts.md 「错误映射」一节
 */
enum class ErrorCode {
  kOk = 0,           ///< 成功。
  kInvalidName,      ///< 名称非法：为空、超过 255 字节、含控制字符或空格，或命中服务通道保留后缀。
  kInvalidConfig,    ///< 配置非法，例如 QosProfile::depth 为 0。
  kDuplicate,        ///< 同进程内已存在同名对象：Node 名，或单 Node 内同类型的端点名。
  kTypeMismatch,     ///< 预留码：请求的消息类型与已创建对象不符；当前实现未产生此码。
  kSerialization,    ///< 序列化或反序列化失败。
  kNoSubscriber,     ///< 仅 Write：通道当前无任何 Reader，消息未发送。
  kQueueFull,        ///< 本地队列已满、消息未发送：Client 的请求队列满时返回；Writer 侧以 WriteStatus::kQueueFull 表达。
  kTimeout,          ///< 在给定截止时间前未等到响应。
  kServiceNotFound,  ///< 注册中心中不存在该服务，或没有可连接的端点。
  kServiceFailed,    ///< 服务端业务失败，与超时、服务不存在严格区分。
  kNotReady,         ///< Init() 未调用、已 Shutdown，或要删除的端点不存在。
  kTransport,        ///< 底层传输或注册中心操作失败；具体原因写入 Result::message。
  kCancelled         ///< 调用方显式撤销，请求未在服务端执行；只在本地兑现，不上线。
};

/// @brief 取 ErrorCode 的可读名，供 Result::message 与 FireflyException 拼装定位线索。
/// @param[in] code 待转换的错误码。
/// @return 静态字符串，永不为空；未知码返回 `"kUnknown"`。
/// @par 线程安全
/// 无状态，可从任意线程调用。
inline const char* ErrorCodeToString(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::kOk:              return "kOk";
    case ErrorCode::kInvalidName:     return "kInvalidName";
    case ErrorCode::kInvalidConfig:   return "kInvalidConfig";
    case ErrorCode::kDuplicate:       return "kDuplicate";
    case ErrorCode::kTypeMismatch:    return "kTypeMismatch";
    case ErrorCode::kSerialization:   return "kSerialization";
    case ErrorCode::kNoSubscriber:    return "kNoSubscriber";
    case ErrorCode::kQueueFull:       return "kQueueFull";
    case ErrorCode::kTimeout:         return "kTimeout";
    case ErrorCode::kServiceNotFound: return "kServiceNotFound";
    case ErrorCode::kServiceFailed:   return "kServiceFailed";
    case ErrorCode::kNotReady:        return "kNotReady";
    case ErrorCode::kTransport:       return "kTransport";
    case ErrorCode::kCancelled:       return "kCancelled";
  }
  return "kUnknown";
}

/**
 * @brief 统一结果类型：成功时 value 有效，失败时 value 为默认值、message 必填。
 *
 * 约定：
 * - 用 `if (r)` / `if (!r)` 判断，不要写 `r.code == ErrorCode::kOk` 样板；
 * - `value_or(fallback)` 在失败时返回 fallback；
 * - `operator->` / `operator*` 仅为降低迁移成本，调用前必须先判 bool。
 *
 * @tparam T 成功时携带的值类型。
 * @sa Result<void> 无返回值特化
 */
template <class T>
struct Result {
  T value{};                        ///< 成功时的值；失败时为默认构造值。
  ErrorCode code = ErrorCode::kOk;  ///< 结果码；kOk 表示成功。
  std::string message;              ///< 失败原因与定位线索；成功时可为空。

  /// @brief 是否成功。
  /// @return code 为 kOk 时返回 true。
  explicit operator bool() const noexcept { return code == ErrorCode::kOk; }

  /// @brief 成功时取值，失败时返回给定回退值。
  /// @param[in] fallback 失败时返回的值。
  /// @return 成功时为 value，失败时为 fallback。
  T value_or(T fallback) const {
    return code == ErrorCode::kOk ? value : std::move(fallback);
  }

  /// @brief 取 value 的指针，仅为简化迁移；调用前必须先判 bool。
  T* operator->() noexcept { return &value; }
  /// @brief 取 value 的只读指针，仅为简化迁移；调用前必须先判 bool。
  const T* operator->() const noexcept { return &value; }
  /// @brief 取 value 的引用，仅为简化迁移；调用前必须先判 bool。
  T& operator*() noexcept { return value; }
  /// @brief 取 value 的只读引用，仅为简化迁移；调用前必须先判 bool。
  const T& operator*() const noexcept { return value; }

  /// @deprecated 仅用于兼容旧写法 `if (r == nullptr)`，改用 `if (!r)`。
  [[deprecated("compare Result via operator bool: if (!r)")]]
  bool operator==(std::nullptr_t) const noexcept { return code != ErrorCode::kOk; }
  /// @deprecated 仅用于兼容旧写法 `if (r != nullptr)`，改用 `if (r)`。
  [[deprecated("compare Result via operator bool: if (r)")]]
  bool operator!=(std::nullptr_t) const noexcept { return code == ErrorCode::kOk; }
};

/**
 * @brief Result 的无返回值特化，用于只关心成败的操作（如 `DeleteXxx`）。
 *
 * 只保留 code、message 与 bool 判定。
 */
template <>
struct Result<void> {
  ErrorCode code = ErrorCode::kOk;  ///< 结果码；kOk 表示成功。
  std::string message;              ///< 失败原因与定位线索；成功时可为空。

  /// @brief 是否成功。
  /// @return code 为 kOk 时返回 true。
  explicit operator bool() const noexcept { return code == ErrorCode::kOk; }

  /// @deprecated 仅用于兼容旧写法 `if (r == nullptr)`，改用 `if (!r)`。
  [[deprecated("compare Result via operator bool: if (!r)")]]
  bool operator==(std::nullptr_t) const noexcept { return code != ErrorCode::kOk; }
  /// @deprecated 仅用于兼容旧写法 `if (r != nullptr)`，改用 `if (r)`。
  [[deprecated("compare Result via operator bool: if (r)")]]
  bool operator!=(std::nullptr_t) const noexcept { return code == ErrorCode::kOk; }
};

/**
 * @brief 服务调用结果：错误码与响应体一起返回。
 *
 * code 与 response 必须一致：code 为 kOk 时 response 通常非空；业务失败
 * （kServiceFailed）与超时、服务不存在、序列化失败之间只靠 code 区分。
 * Service 与 Client 共用此类型，保证错误码端到端可传递。
 *
 * @tparam Response 响应消息类型。
 */
template <class Response>
struct ServiceResult {
  ErrorCode code = ErrorCode::kOk;          ///< 结果码；kOk 表示调用成功。
  std::shared_ptr<Response> response;       ///< 响应体；失败时可为空。

  /// @brief 是否成功且携带响应体。
  /// @return code 为 kOk 且 response 非空时返回 true。
  explicit operator bool() const noexcept {
    return code == ErrorCode::kOk && response != nullptr;
  }

  /// @brief 取 response 的指针，仅为简化迁移；调用前必须先判 bool。
  Response* operator->() const noexcept { return response.get(); }

  /// @deprecated 仅用于兼容旧写法 `if (r == nullptr)`，改用 `if (!r)`。
  [[deprecated("compare ServiceResult via operator bool: if (!r)")]]
  bool operator==(std::nullptr_t) const noexcept { return !static_cast<bool>(*this); }
  /// @deprecated 仅用于兼容旧写法 `if (r != nullptr)`，改用 `if (r)`。
  [[deprecated("compare ServiceResult via operator bool: if (r)")]]
  bool operator!=(std::nullptr_t) const noexcept { return static_cast<bool>(*this); }
};

/**
 * @brief 异步接口失败时在 future 中兑现的异常类型。
 *
 * 同步接口不用异常，统一返回 Result / ServiceResult。
 */
class FireflyException : public std::runtime_error {
 public:
  /// @brief 构造异常。
  /// @param[in] code    失败原因码，与同步接口的 ErrorCode 语义一致。
  /// @param[in] message 定位线索，至少包含失败的操作与对象名。
  FireflyException(ErrorCode code, const std::string& message)
      : std::runtime_error(message), code_(code) {}

  /// @brief 取失败原因码。
  /// @return 构造时传入的 ErrorCode。
  ErrorCode code() const noexcept { return code_; }

 private:
  ErrorCode code_;
};

/**
 * @brief 消息回调的上下文信息。
 *
 * sequence 是**本 Reader** 收到的序号，从 1 起递增；它是单订阅者局部序，
 * 不是全局序，也不在进程间保持单调。receive_time 基于 steady_clock，
 * 只用于测量相对耗时，不可转换为日历时间。
 */
struct MessageContext {
  std::string channel;                              ///< 消息所在通道名。
  std::uint64_t sequence = 0;                        ///< 本 Reader 内的到达序号，从 1 起递增。
  std::chrono::steady_clock::time_point receive_time{};  ///< 收到消息的单调时钟时刻，用于测相对延迟。
};

/**
 * @brief 回调返回值：只区分消费与丢弃，不引入重试或限流语义。
 */
enum class CallbackResult : int {
  kOk = 0,      ///< 已消费；计入 Stats().recv。
  kDropped = 1  ///< 本条不消费；与队列满丢弃合并计入 Stats().dropped。
};

/**
 * @brief 单次 Write 的受理结果。
 *
 * 单独枚举而不复用 ErrorCode，是因为 Write 不产生细粒度错误信息。
 * 各值的精确含义与送达边界见 writer.h 的 Write() 文档。
 */
enum class WriteStatus {
  kAccepted,            ///< 已交给本地传输层，不构成对端收到的承诺。
  kNoSubscriber,        ///< 通道当前无 Reader，消息未发送。
  kQueueFull,           ///< 本地发送队列已满，消息未发送。
  kSerializationError,  ///< 消息序列化失败，消息未发送。
  kTransportError       ///< Writer 未初始化、已关停，或底层传输失败。
};

/// @brief 便于灰度迁移：旧写法 `if (!writer->Write(msg))` 继续可用。
/// @param[in] s 待判定的写入结果。
/// @return kAccepted 返回 false，其余情况返回 true。
inline bool operator!(WriteStatus s) noexcept {
  return s != WriteStatus::kAccepted;
}

/**
 * @brief 端点的实时统计快照。
 *
 * dropped 的统计口径按端点角色不同：Writer 统计本地队列满丢弃，Reader 统计
 * 回调返回 kDropped 的条数（观察队列被深度截断的旧消息不计入）。两者都不含
 * 序列化与传输错误。queue_depth 是 Observe 队列的**实时积压条数**而非配置值；
 * Writer、Service、Client 无 Observe 队列，该字段恒为 0。
 * mps 限流尚未实现，本结构只提供累计计数，不提供速率。
 */
struct EndpointStats {
  std::uint64_t sent = 0;         ///< 成功交给传输层的消息条数。
  std::uint64_t recv = 0;         ///< 成功收到并交给回调或写入 Observe 队列的条数；回调返回 kDropped 也计入。
  std::uint64_t dropped = 0;      ///< 丢弃条数：Writer 统计本地队列满丢弃，Reader 统计回调返回 kDropped，Client 不统计。
  std::uint64_t queue_depth = 0;  ///< Observe 队列当前积压条数；非 Reader 端点恒为 0。
};

/**
 * @brief 节点与读写者之间的物理位置关系。
 */
enum Relation : std::uint8_t {
  NO_RELATION = 0,  ///< 关系未知。
  DIFF_HOST,        ///< 不同主机。
  DIFF_PROC,        ///< 同主机、不同进程。
  SAME_PROC,        ///< 同一进程。
};

/**
 * @brief QoS 历史策略。
 */
enum class QosHistoryPolicy : std::uint8_t {
  kKeepLast,  ///< 只保留最近 depth 条，超出时丢弃最旧的一条。
  kKeepAll    ///< 声明保留全部；当前实现未按此策略扩展缓存，行为等同 kKeepLast。
};

/**
 * @brief QoS 可靠性策略。
 */
enum class QosReliabilityPolicy : std::uint8_t {
  kReliable,   ///< 声明可靠投递；当前实现不提供确认与重传，行为与 kBestEffort 相同。
  kBestEffort  ///< 至多一次投递，无确认、无重传、无持久化。
};

/**
 * @brief QoS 持久性策略。
 */
enum class QosDurabilityPolicy : std::uint8_t {
  kVolatile,        ///< 不保证晚加入的订阅者收到历史消息。
  kTransientLocal   ///< 声明为发布者本地持久；当前实现未实现，行为等同 kVolatile。
};

/**
 * @brief 通道 QoS 配置。
 *
 * 策略名描述目标语义，逐字段的实际生效情况见各枚举值与下列字段注释。
 * 当前唯一真正影响行为的字段是 depth。
 *
 * @sa docs/architecture/known-gaps.md 「QoS 仅 depth 生效」
 */
struct QosProfile {
  QosHistoryPolicy history = QosHistoryPolicy::kKeepLast;  ///< 历史策略；只影响 depth 的解释，不单独生效。
  std::uint32_t depth = 1;      ///< 保留样本数，须大于 0；Reader 的 Observe 队列即按此值截断。
  std::uint32_t mps = 0;        ///< 每秒最大样本数，0 表示不限；当前无限流实现，不生效。
  QosReliabilityPolicy reliability = QosReliabilityPolicy::kReliable;  ///< 可靠性策略；当前两种取值行为相同。
  QosDurabilityPolicy durability = QosDurabilityPolicy::kVolatile;    ///< 持久性策略；当前两种取值行为相同。
};

/**
 * @brief 节点与角色属性，Apollo RoleAttributes 的轻量 C++ 等价物。
 */
struct RoleAttributes {
  std::string node_name;            ///< 所属节点名。
  std::string channel_name;         ///< 通道名；服务端点填服务名。
  std::string host_name;            ///< 主机名。
  std::string host_ip;              ///< 主机 IP。
  std::int32_t process_id = -1;     ///< 进程 ID，-1 表示未填。
  std::uint64_t node_id = 0;        ///< 节点 ID。
  std::uint64_t channel_id = 0;     ///< 通道 ID。
  std::string message_type;         ///< 消息类型的 protobuf 全名。
  QosProfile qos_profile;           ///< 该通道的 QoS 配置。
};

/// @brief 服务请求通道的名称后缀。
/// @note 该后缀为保留字：通道名命中它会被判定为 kInvalidName。
static const char SRV_CHANNEL_REQ_SUFFIX[] = "__SRV__REQUEST";
/// @brief 服务响应通道的名称后缀。
/// @note 该后缀为保留字：通道名命中它会被判定为 kInvalidName。
static const char SRV_CHANNEL_RES_SUFFIX[] = "__SRV__RESPONSE";

}  // namespace firefly

#endif  // FIREFLY_COMMON_TYPES_H_
