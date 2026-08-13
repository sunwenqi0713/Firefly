#ifndef FIREFLY_COMMON_TYPES_H_
#define FIREFLY_COMMON_TYPES_H_

#include <cstdint>
#include <string>

namespace firefly {

// 空类型，用于无数据的占位
class NullType {};

// 内部返回值定义
enum ReturnCode { SUCC = 0, FAIL = 1 };

// 描述节点/读写者之间的物理关系
enum Relation : std::uint8_t {
  NO_RELATION = 0,  // 无关系（不同主机）
  DIFF_HOST,        // 不同主机
  DIFF_PROC,        // 同主机、不同进程
  SAME_PROC,        // 同进程
};

// QoS 策略（参考 DDS/RTPS 语义）
enum class QosHistoryPolicy : std::uint8_t { kKeepLast, kKeepAll };
enum class QosReliabilityPolicy : std::uint8_t { kReliable, kBestEffort };
enum class QosDurabilityPolicy : std::uint8_t { kVolatile, kTransientLocal };

// 通道 QoS 配置
struct QosProfile {
  QosHistoryPolicy history = QosHistoryPolicy::kKeepLast;
  std::uint32_t depth = 1;    // KEEP_LAST 时保留的样本数
  std::uint32_t mps = 0;      // 每秒最大样本数，0 表示不限制
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
