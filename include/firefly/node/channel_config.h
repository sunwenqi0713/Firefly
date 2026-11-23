#ifndef FIREFLY_NODE_CHANNEL_CONFIG_H_
#define FIREFLY_NODE_CHANNEL_CONFIG_H_

#include <cstdint>
#include <string>

#include "firefly/common/types.h"

namespace firefly {

// Writer / Reader 共用的通道配置
struct ChannelConfig {
  std::string channel_name;
  QosProfile qos_profile;
};

// Reader 扩展配置（阶段三：缓存深度统一由 qos_profile.depth 决定）。
// 原 pending_queue_size 已删除，避免两个字段竞争同一语义。
struct ReaderConfig {
  std::string channel_name;
  QosProfile qos_profile;
};

}  // namespace firefly

#endif  // FIREFLY_NODE_CHANNEL_CONFIG_H_