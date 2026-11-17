// stats_test.cpp：EndpointStats 实时积压、dropped 统计与假 getter 消除。
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>

#include "firefly/firefly.h"
#include "test_messages.pb.h"

using firefly::CallbackResult;
using firefly::EndpointStats;
using firefly::test::Chatter;

// 编译期确认假 getter 已删除（原实现返回配置值而非实时深度）。
template <typename T, typename = void>
struct HasPendingQueueSize : std::false_type {};
template <typename T>
struct HasPendingQueueSize<
    T, std::void_t<decltype(&T::PendingQueueSize)>> : std::true_type {};
static_assert(!HasPendingQueueSize<firefly::Reader<Chatter>>::value,
              "PendingQueueSize fake getter must be removed");

static std::shared_ptr<Chatter> Msg(long long seq) {
  auto msg = std::make_shared<Chatter>();
  msg->set_seq(seq);
  return msg;
}

int main() {
  firefly::Init("stats_test");
  auto node = firefly::CreateNode("stats_node").value;
  if (!node) return 1;

  // depth=1，灌 2 条：保留最新 1 条，queue_depth 实时反映 1。
  firefly::ReaderConfig cfg;
  cfg.channel_name = "stats_ch";
  cfg.qos_profile.depth = 1;
  auto reader = node->CreateReader<Chatter>(cfg, nullptr).value;
  if (!reader) return 2;
  auto writer = node->CreateWriter<Chatter>("stats_ch").value;
  if (!writer) return 3;
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  if (writer->Write(Msg(1)) != firefly::WriteStatus::kAccepted) return 4;
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  if (writer->Write(Msg(2)) != firefly::WriteStatus::kAccepted) return 5;
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  EndpointStats rs = reader->Stats();
  if (rs.recv != 2) return 6;
  if (rs.queue_depth != 1) return 7;
  if (reader->GetAllObserved().size() != 1) return 8;
  if (reader->GetAllObserved().front()->seq() != 2) return 9;

  EndpointStats ws = writer->Stats();
  if (ws.sent != 2) return 10;
  // Writer 的 queue_depth 恒为 0（无 Observe 队列）
  if (ws.queue_depth != 0) return 11;

  // kDropped 与队列满合并计数：用回调模式验证 kDropped 分支
  std::atomic<int> seen{0};
  auto drop_reader = node->CreateReader<Chatter>(
      "stats_drop_ch",
      [&](const firefly::MessageContext&,
          const std::shared_ptr<const Chatter>&) {
        ++seen;
        return seen.load() == 1 ? CallbackResult::kDropped
                                : CallbackResult::kOk;
      }).value;
  auto drop_writer = node->CreateWriter<Chatter>("stats_drop_ch").value;
  if (!drop_reader || !drop_writer) return 20;
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  drop_writer->Write(Msg(1));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  drop_writer->Write(Msg(2));
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  if (drop_reader->Stats().dropped != 1) return 21;

  firefly::Clear();
  return 0;
}
