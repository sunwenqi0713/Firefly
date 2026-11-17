#ifndef FIREFLY_MESSAGE_MESSAGE_TRAITS_H_
#define FIREFLY_MESSAGE_MESSAGE_TRAITS_H_

// ---------------------------------------------------------------------------
// 消息 schema 演进规则（显式契约，阶段一）
// ---------------------------------------------------------------------------
// 1) 新增字段必须声明为 optional（proto3 中即"不使用 required 语义"），
//    保证旧读者跳过未知字段、新读者对缺失字段取默认值。
// 2) 禁止修改已有字段的字段号、类型和 wire type；删除字段须以 reserved 保留
//    其字段号，防止后续复用导致新旧数据被错误解析。
// 3) TypeName() 返回 protobuf 的 full_name()，当前**不包含版本号**。
//    版本协商 / 多版本共存是已知缺口，phase2+ 处理；本阶段不实现版本号。
// 4) 空载荷（0 字节）是合法消息：表示"所有字段取默认值"的 protobuf 消息。
//    不得以"序列化结果为空"作为失败判据。
// ---------------------------------------------------------------------------

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

  // 消息类型名（protobuf 完整描述名），用于拓扑注册与按类型匹配。
  // 注意：不含版本，见文件头 schema 演进规则第 3 条。
  static std::string TypeName() { return MessageT::descriptor()->full_name(); }

  // 序列化：空载荷是合法的"全默认字段"消息，不再视为失败。
  static bool Serialize(const MessageT& msg, std::string* output) {
    if (output == nullptr) {
      return false;
    }
    output->clear();
    return msg.SerializeToString(output);
  }

  // 反序列化：0 字节输入合法（= 全默认字段消息），仅保留长度上限检查。
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
