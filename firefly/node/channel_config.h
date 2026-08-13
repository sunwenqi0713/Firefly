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

// Reader 扩展配置：在通道配置基础上额外支持消息缓存队列深度
struct ReaderConfig {
  std::string channel_name;
  QosProfile qos_profile;
  // 收到消息后缓存队列的深度，超出后丢弃最旧消息
  std::uint32_t pending_queue_size = 1;
};

}  // namespace firefly

#endif  // FIREFLY_NODE_CHANNEL_CONFIG_H_