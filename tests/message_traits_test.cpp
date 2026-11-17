// message_traits_test.cpp：schema 演进规则的测试期校验（阶段三任务 5-13）。
// 规则：新增字段必须 optional；required 会破坏演进，本测试断言无 required 字段。
// 不实现运行时版本协商；TypeName() 仍为 full_name()，不含版本号。
#include <string>

#include "firefly/message/message_traits.h"
#include "test_messages.pb.h"

using firefly::message::MessageTraits;
using firefly::test::CalcRequest;
using firefly::test::CalcResponse;
using firefly::test::Chatter;

namespace {

template <typename M>
bool HasNoRequiredFields() {
  const auto* descriptor = M::descriptor();
  for (int i = 0; i < descriptor->field_count(); ++i) {
    if (descriptor->field(i)->is_required()) return false;
  }
  return true;
}

}  // namespace

int main() {
  if (!HasNoRequiredFields<Chatter>()) return 1;
  if (!HasNoRequiredFields<CalcRequest>()) return 2;
  if (!HasNoRequiredFields<CalcResponse>()) return 3;

  if (MessageTraits<Chatter>::TypeName() != "firefly.test.Chatter") return 4;
  if (MessageTraits<CalcRequest>::TypeName() != "firefly.test.CalcRequest") return 5;

  // 空载荷 = 全默认字段消息，仍是合法输入（阶段一契约）
  Chatter decoded;
  if (!MessageTraits<Chatter>::Deserialize("", &decoded)) return 6;
  if (decoded.seq() != 0 || !decoded.content().empty()) return 7;

  std::string bytes;
  if (!MessageTraits<Chatter>::Serialize(decoded, &bytes)) return 8;
  // proto3 全默认字段序列化结果为空，这是合法而非失败。
  if (!bytes.empty()) return 9;
  return 0;
}
