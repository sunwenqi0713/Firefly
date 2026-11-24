#ifndef FIREFLY_NODE_CHANNEL_CONFIG_H_
#define FIREFLY_NODE_CHANNEL_CONFIG_H_

/// @file channel_config.h
/// @brief 通道与订阅者的配置结构。

#include <cstdint>
#include <string>

#include "firefly/common/types.h"

namespace firefly {

/**
 * @brief Writer 与 Reader 共用的通道配置。
 */
struct ChannelConfig {
  std::string channel_name;  ///< 通道名；不得为空、过长或含控制字符。
  QosProfile qos_profile;    ///< 该通道的 QoS；depth 须大于 0。
};

/**
 * @brief Reader 的完整配置。
 *
 * 缓存深度只由 qos_profile.depth 决定，不再单独提供队列长度字段，避免同一
 * 语义存在两个来源。
 */
struct ReaderConfig {
  std::string channel_name;  ///< 通道名；不得为空、过长或含控制字符。
  QosProfile qos_profile;    ///< 该通道的 QoS；depth 同时决定观察队列深度。
};

}  // namespace firefly

#endif  // FIREFLY_NODE_CHANNEL_CONFIG_H_