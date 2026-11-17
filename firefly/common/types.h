#ifndef FIREFLY_COMMON_TYPES_H_
#define FIREFLY_COMMON_TYPES_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace firefly {

// 空类型，用于无数据的占位
class NullType {};

// ---------------------------------------------------------------------------
// 错误码与结果类型（阶段一：通信语义与错误模型）
// ---------------------------------------------------------------------------

// 旧错误码：保留一个版本供二进制消费方灰度，新代码一律使用 ErrorCode/Result。
enum [[deprecated("Firefly: use ErrorCode with Result<T> instead")]] ReturnCode {
  SUCC = 0,
  FAIL = 1
};

// 统一错误码。每个码都有可触发路径，禁止再用 bool/nullptr 折叠失败原因。
enum class ErrorCode {
  kOk = 0,
  kInvalidName,        // 通道/节点/服务名非法（空、含非法字符、命中服务后缀保留字）
  kInvalidConfig,      // 配置非法（depth==0、mps<0 等）
  kDuplicate,          // 名称重复（本阶段仅 Reader 查重落此码）
  kTypeMismatch,       // 类型不符（GetReader 的 dynamic_pointer_cast 失败等）
  kSerialization,      // 序列化/反序列化失败
  kNoSubscriber,       // 仅 Write：当前通道无任何订阅者
  kQueueFull,          // 发送队列满（本阶段仅 nng EAGAIN 映射）
  kTimeout,            // RPC 等待响应超时
  kServiceNotFound,    // 服务端不存在（注册中心无该服务）
  kServiceFailed,      // 服务端业务失败（与前两者严格区分）
  kNotReady,           // Init 未调用 / 端点已 Shutdown
  kTransport,          // nng 层错误，底层码写入 Result::message
  kCancelled           // 请求被显式 Cancel（阶段二：RequestHandle::Cancel）
};

// ErrorCode 可读名，供 Result::message 与 FireflyException 拼装定位线索。
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

// 统一结果类型：失败时 value 为默认值（shared_ptr 为 nullptr），message 必填。
// 约定：
//   - 用 if (r) / if (!r) 判断，不要写 r.code == ErrorCode::kOk 样板；
//   - value_or(fallback) 在失败时返回 fallback；
//   - operator-> / operator* 仅为降低迁移成本，调用前必须先判 bool。
template <class T>
struct Result {
  T value{};
  ErrorCode code = ErrorCode::kOk;
  std::string message;

  explicit operator bool() const noexcept { return code == ErrorCode::kOk; }

  T value_or(T fallback) const {
    return code == ErrorCode::kOk ? value : std::move(fallback);
  }

  T* operator->() noexcept { return &value; }
  const T* operator->() const noexcept { return &value; }
  T& operator*() noexcept { return value; }
  const T& operator*() const noexcept { return value; }

  // 源码级灰度：旧写法 `if (create_result == nullptr)` 仍可编译（仅比较 code）。
  // 注意：二进制 ABI 不兼容（返回类型由 shared_ptr 改为 Result），故同时提供
  // 源码兼容与返回码兼容，真正二进制灰度需靠版本号/独立 soname，见交付说明。
  [[deprecated("compare Result via operator bool: if (!r)")]]
  bool operator==(std::nullptr_t) const noexcept { return code != ErrorCode::kOk; }
  [[deprecated("compare Result via operator bool: if (r)")]]
  bool operator!=(std::nullptr_t) const noexcept { return code == ErrorCode::kOk; }
};

// void 特化：用于无返回值的操作（如 DeleteXxx）。只保留 code/message 与 bool 判定。
template <>
struct Result<void> {
  ErrorCode code = ErrorCode::kOk;
  std::string message;

  explicit operator bool() const noexcept { return code == ErrorCode::kOk; }

  [[deprecated("compare Result via operator bool: if (!r)")]]
  bool operator==(std::nullptr_t) const noexcept { return code != ErrorCode::kOk; }
  [[deprecated("compare Result via operator bool: if (r)")]]
  bool operator!=(std::nullptr_t) const noexcept { return code == ErrorCode::kOk; }
};

// 服务调用结果：code 与 response 必须一致 —— code != kOk 时 response 可为空，
// 但业务失败(kServiceFailed)与超时/服务不存在/序列化失败必须靠 code 区分。
// Service 与 Client 共用此类型，保证错误码端到端可传递。
template <class Response>
struct ServiceResult {
  ErrorCode code = ErrorCode::kOk;
  std::shared_ptr<Response> response;

  explicit operator bool() const noexcept {
    return code == ErrorCode::kOk && response != nullptr;
  }
  Response* operator->() const noexcept { return response.get(); }

  // 源码级灰度：旧写法 `if (resp == nullptr)` 仍可编译。
  [[deprecated("compare ServiceResult via operator bool: if (!r)")]]
  bool operator==(std::nullptr_t) const noexcept { return !static_cast<bool>(*this); }
  [[deprecated("compare ServiceResult via operator bool: if (r)")]]
  bool operator!=(std::nullptr_t) const noexcept { return static_cast<bool>(*this); }
};

// 异步接口失败时在 future 中兑现的异常类型。
// 同步接口不用异常，统一返回 Result / ServiceResult。
class FireflyException : public std::runtime_error {
 public:
  FireflyException(ErrorCode code, const std::string& message)
      : std::runtime_error(message), code_(code) {}

  ErrorCode code() const noexcept { return code_; }

 private:
  ErrorCode code_;
};

// 描述节点/读写者之间的物理关系
enum Relation : std::uint8_t {
  NO_RELATION = 0,  // 无关系（不同主机）
  DIFF_HOST,        // 不同主机
  DIFF_PROC,        // 同主机、不同进程
  SAME_PROC,        // 同进程
};

// QoS 策略（参考 DDS/RTPS 语义）
//
// 注意：策略名与"本实现实际行为"并不完全一致，逐字段如实标注：
//   history / depth   : 当前仅 ReaderConfig::pending_queue_size 生效（收端保留样本数）；
//                       QosProfile::depth 本阶段未参与行为，TODO(phase2) 接入。
//   mps               : 声明每秒最大样本数，但本阶段无限流/背压实现，不生效。
//   reliability       : kBestEffort 是当前 nng PUB/SUB 的实际行为（at-most-once）；
//                       kReliable 本阶段不提供 ack/重传，行为与 kBestEffort 相同。
//                       TODO(phase2): 传输层可靠性增强。
//   durability        : kVolatile 是当前行为；kTransientLocal 未实现，当前等同 volatile。
//                       TODO(phase2): 未实现，当前等同 volatile。
// 阶段一不修改任何传输行为，只把现状落成显式契约。
enum class QosHistoryPolicy : std::uint8_t { kKeepLast, kKeepAll };
enum class QosReliabilityPolicy : std::uint8_t { kReliable, kBestEffort };
enum class QosDurabilityPolicy : std::uint8_t { kVolatile, kTransientLocal };

// 通道 QoS 配置（字段实际生效情况见上方注释）
struct QosProfile {
  QosHistoryPolicy history = QosHistoryPolicy::kKeepLast;
  std::uint32_t depth = 1;    // KEEP_LAST 保留样本数；本阶段未生效，见上方注释
  std::uint32_t mps = 0;      // 每秒最大样本数，0 不限；本阶段无限流实现
  QosReliabilityPolicy reliability = QosReliabilityPolicy::kReliable;
  QosDurabilityPolicy durability = QosDurabilityPolicy::kVolatile;
};

// 节点/角色属性（Apollo RoleAttributes 的轻量 C++ 等价物）
struct RoleAttributes {
  std::string node_name;
  std::string channel_name;
  std::string host_name;
  std::string host_ip;
  std::int32_t process_id = -1;
  std::uint64_t node_id = 0;
  std::uint64_t channel_id = 0;
  std::string message_type;
  QosProfile qos_profile;
};

// 服务在底层通过两条通道实现，名称后缀
static const char SRV_CHANNEL_REQ_SUFFIX[] = "__SRV__REQUEST";
static const char SRV_CHANNEL_RES_SUFFIX[] = "__SRV__RESPONSE";

}  // namespace firefly

#endif  // FIREFLY_COMMON_TYPES_H_
