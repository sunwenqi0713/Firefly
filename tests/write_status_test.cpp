// write_status_test.cpp：kAccepted / kNoSubscriber / kQueueFull 三态可区分。
#include <chrono>
#include <memory>
#include <thread>

#include "firefly/detail/endpoint_api.h"
#include "firefly/firefly.h"
#include "test_messages.pb.h"

using firefly::WriteStatus;
using firefly::test::Chatter;

int main() {
  firefly::Init("write_status_test");
  auto node = firefly::CreateNode("write_status_node").value;
  if (!node) return 1;

  Chatter msg;
  msg.set_seq(1);
  msg.set_content("hello");

  // kNoSubscriber：通道上无任何 Reader
  auto no_sub_writer = node->CreateWriter<Chatter>("ch_no_subscriber").value;
  if (!no_sub_writer) return 10;
  if (no_sub_writer->Write(msg) != WriteStatus::kNoSubscriber) return 11;

  // kAccepted：注册 Reader 后，消息成功交给本地传输层
  auto reader = node->CreateReader<Chatter>("ch_no_subscriber").value;
  if (!reader) return 20;
  std::this_thread::sleep_for(std::chrono::milliseconds(500));  // 等待发现/dial
  if (!reader->HasWriter()) return 21;
  if (no_sub_writer->Write(msg) != WriteStatus::kAccepted) return 22;

  // kQueueFull：协议层 EAGAIN 映射为 kQueueFull，与另两态严格区分
  if (firefly::detail::MapSendError(
          firefly::detail::TransferStatus::kWouldBlock) !=
      WriteStatus::kQueueFull) return 30;
  if (WriteStatus::kQueueFull == WriteStatus::kAccepted) return 31;

  // depth=1 灌 2 条：当前实现的实际行为是"收端保留最新一条"，
  // Write 仍返回 kAccepted —— nng PUB 无背压，kQueueFull 在收端不产生。
  // 这一现状如实记录在 writer.h 头注释中。
  firefly::ReaderConfig cfg;
  cfg.channel_name = "ch_depth_one";
  cfg.qos_profile.depth = 1;
  auto depth_reader = node->CreateReader<Chatter>(cfg).value;
  auto depth_writer = node->CreateWriter<Chatter>("ch_depth_one").value;
  if (!depth_reader || !depth_writer) return 40;
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  Chatter m1, m2;
  m1.set_seq(1);
  m2.set_seq(2);
  if (depth_writer->Write(m1) != WriteStatus::kAccepted) return 41;
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  if (depth_writer->Write(m2) != WriteStatus::kAccepted) return 42;
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  auto all = depth_reader->GetAllObserved();
  if (all.size() != 1 || all[0]->seq() != 2) return 43;

  firefly::Clear();
  return 0;
}
