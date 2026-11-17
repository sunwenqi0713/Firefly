// empty_message_test.cpp：全默认字段 protobuf 消息端到端 Write→Read 往返。
// 契约：0 字节载荷 = 全默认字段消息，合法（见 message_traits.h 头部注释）。
#include <chrono>
#include <memory>
#include <thread>

#include "firefly/firefly.h"
#include "firefly/message/message_traits.h"
#include "test_messages.pb.h"

using firefly::test::Chatter;

int main() {
  // 1) 序列化层：全默认消息序列化结果为空串，但返回 true
  Chatter empty;
  std::string bytes;
  if (!firefly::message::MessageTraits<Chatter>::Serialize(empty, &bytes)) return 1;
  if (!bytes.empty()) return 2;

  // 2) 反序列化层：空串合法，得到全默认消息
  Chatter decoded;
  if (!firefly::message::MessageTraits<Chatter>::Deserialize(bytes, &decoded)) return 3;
  if (decoded.seq() != 0 || !decoded.content().empty()) return 4;

  firefly::Init("empty_message_test");
  auto node = firefly::CreateNode("empty_message_node");
  if (!node) return 10;

  auto writer = node->CreateWriter<Chatter>("ch_empty").value;
  auto reader = node->CreateReader<Chatter>("ch_empty").value;
  if (!writer || !reader) return 11;
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  // 3) 端到端：空消息经 PUB/SUB 往返
  if (writer->Write(empty) != firefly::WriteStatus::kAccepted) return 20;
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  if (!reader->HasReceived()) return 21;
  auto all = reader->GetAllObserved();
  if (all.size() != 1) return 22;
  if (all[0]->seq() != 0 || !all[0]->content().empty()) return 23;

  firefly::Clear();
  return 0;
}
