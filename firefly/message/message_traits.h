#ifndef FIREFLY_MESSAGE_MESSAGE_TRAITS_H_
#define FIREFLY_MESSAGE_MESSAGE_TRAITS_H_

#include <google/protobuf/message.h>

#include <limits>
#include <string>
#include <type_traits>

namespace firefly {
namespace message {

// 序列化能力判定：依赖 protobuf，所有消息类型必须派生自
// google::protobuf::Message（由 protoc 生成）。
template <typename MessageT>
struct HasSerializer {
  static constexpr bool value =
      std::is_base_of<google::protobuf::Message, MessageT>::value;
};

template <typename MessageT>
struct MessageTraits {
  static_assert(HasSerializer<MessageT>::value,
                "MessageT must derive from google::protobuf::Message.");

  // 消息类型名（protobuf 完整描述名），用于拓扑注册与按类型匹配
  static std::string TypeName() { return MessageT::descriptor()->full_name(); }

  // 序列化 / 反序列化
  static bool Serialize(const MessageT& msg, std::string* output) {
    if (output == nullptr) {
      return false;
    }
    output->clear();
    return msg.SerializeToString(output) && !output->empty();
  }

  static bool Deserialize(const std::string& input, MessageT* output) {
    if (output == nullptr) {
      return false;
    }
    return input.size() <= static_cast<size_t>(
                               std::numeric_limits<int>::max()) &&
           input.size() != 0 && output->ParseFromArray(input.data(),
                                                       static_cast<int>(
                                                           input.size()));
  }
};

}  // namespace message
}  // namespace firefly

#endif  // FIREFLY_MESSAGE_MESSAGE_TRAITS_H_