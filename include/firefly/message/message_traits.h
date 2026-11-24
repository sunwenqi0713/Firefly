#ifndef FIREFLY_MESSAGE_MESSAGE_TRAITS_H_
#define FIREFLY_MESSAGE_MESSAGE_TRAITS_H_

/// @file message_traits.h
/// @brief 消息序列化适配与 schema 演进规则。
///
/// 演进规则（对全部消息类型生效）：
/// 1. 新增字段必须是 optional 语义，保证旧读者跳过未知字段、新读者对缺失字段
///    取默认值。
/// 2. 禁止修改已有字段的字段号、类型与 wire type；删除字段须用 reserved 保留
///    其字段号，防止后续复用导致新旧数据被错误解析。
/// 3. TypeName() 返回 protobuf 的 full_name()，不含版本号；版本协商与多版本
///    共存不在实现范围内。
/// 4. 空载荷（0 字节）是合法消息，表示"所有字段取默认值"；不得以"序列化结果
///    为空"作为失败判据。
///
/// 规则 1 由测试守护：新增消息或字段后运行消息契约测试，任何 required 字段
/// 都会使测试失败。见 docs/architecture/contracts.md §9。

#include <google/protobuf/message.h>

#include <limits>
#include <string>
#include <type_traits>

namespace firefly {
namespace message {

/**
 * @brief 判定类型是否可作为消息使用。
 *
 * @tparam MessageT 待判定的类型。
 * @note 要求派生自 google::protobuf::Message，即由 protoc 生成的消息类。
 */
template <typename MessageT>
struct HasSerializer {
  static constexpr bool value =
      std::is_base_of<google::protobuf::Message, MessageT>::value;  ///< 判定结果。
};

/**
 * @brief 消息类型的序列化适配器。
 *
 * @tparam MessageT 消息类型，须派生自 google::protobuf::Message。
 */
template <typename MessageT>
struct MessageTraits {
  static_assert(HasSerializer<MessageT>::value,
                "MessageT must derive from google::protobuf::Message.");

  /// @brief 取消息类型名，用于拓扑注册与按类型匹配。
  /// @return protobuf 的完整描述名；不含版本信息。
  static std::string TypeName() {
    return std::string(MessageT::descriptor()->full_name());
  }

  /// @brief 序列化消息。
  /// @param[in]  msg    待序列化消息。
  /// @param[out] output 输出缓冲；非空，内容会被清空后写入。
  /// @return 成功返回 true；output 为空指针或 protobuf 序列化失败时返回 false。
  /// @note 序列化结果可以是 0 字节，这是合法的"全默认字段"消息。
  static bool Serialize(const MessageT& msg, std::string* output) {
    if (output == nullptr) {
      return false;
    }
    output->clear();
    return msg.SerializeToString(output);
  }

  /// @brief 反序列化消息。
  /// @param[in]  input  字节序列；允许为空（表示全默认字段消息）。
  /// @param[out] output 目标消息；非空，已有内容会被覆盖。
  /// @return 成功返回 true；output 为空指针、输入超出 int 上界或解析失败时
  ///         返回 false。
  static bool Deserialize(const std::string& input, MessageT* output) {
    if (output == nullptr) {
      return false;
    }
    return input.size() <= static_cast<size_t>(
                               std::numeric_limits<int>::max()) &&
           output->ParseFromArray(input.data(),
                                  static_cast<int>(input.size()));
  }
};

}  // namespace message
}  // namespace firefly

#endif  // FIREFLY_MESSAGE_MESSAGE_TRAITS_H_
