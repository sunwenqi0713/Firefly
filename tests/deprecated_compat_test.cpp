// deprecated_compat_test.cpp：验证旧 API 写法经 deprecated 路径仍可编译。
// 这是源码级灰度保证；二进制 ABI 灰度需靠版本化 soname（见交付说明）。
#include <chrono>
#include <memory>

#include "firefly/firefly.h"
#include "test_messages.pb.h"

using firefly::test::Chatter;

int main() {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)  // C4996: deprecated
#endif

  // 旧错误码仍存在（仅 deprecated，未删除）
  firefly::ReturnCode old_rc = firefly::SUCC;
  (void)old_rc;

  firefly::Init("deprecated_compat_test");
  auto node = firefly::CreateNode("compat_node");

  // 旧写法：用 == nullptr / != nullptr 判断 Create 结果
  auto writer_r = node->CreateWriter<Chatter>("ch_compat");
  if (writer_r == nullptr) return 10;
  auto reader_r = node->CreateReader<Chatter>("ch_compat");
  if (reader_r != nullptr) {
    // 旧写法：!Write() 通过 operator! 兼容
    Chatter msg;
    msg.set_seq(1);
    (void)writer_r.value->Write(msg);
  }

  firefly::Clear();
#ifdef _MSC_VER
#pragma warning(pop)
#endif
  return 0;
}
