// Reader 单元测试：覆盖 Observe / 回调两种模式及缓存深度、在线关系等 API。
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>

#include "firefly/firefly.h"
#include "test_messages.pb.h"

using firefly::test::Chatter;

namespace {

constexpr auto kSettle = std::chrono::milliseconds(600);

void SleepMs(int ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

}  // namespace

int main() {
  firefly::Init("reader_test");
  auto node = firefly::CreateNode("reader_test_node").value;

  // ---------- 回调模式 ----------
  std::atomic<int> cb_count{0};
  std::atomic<long long> cb_seq{-1};
  auto cb_reader = node->CreateReader<Chatter>(
      "channel/callback",
      [&](const std::shared_ptr<Chatter>& msg) {
        ++cb_count;
        cb_seq.store(msg->seq());
      }).value;
  if (!cb_reader) return 10;
  if (!cb_reader->IsInit()) return 11;

  auto writer = node->CreateWriter<Chatter>("channel/callback").value;
  if (!writer) return 12;

  SleepMs(500);  // 等订阅建立
  if (!cb_reader->HasWriter()) return 13;
  if (!writer->HasReader()) return 14;

  Chatter msg;
  msg.set_seq(100);
  msg.set_content("hello");
  if (!writer->Write(msg)) return 15;
  SleepMs(kSettle.count());
  if (cb_count.load() != 1) return 16;
  if (cb_seq.load() != 100) return 17;
  if (!cb_reader->HasReceived()) return 18;

  // ---------- Observe 模式：历史深度 / 拉取 ----------
  firefly::ReaderConfig obs_cfg;
  obs_cfg.channel_name = "channel/observe";
  obs_cfg.pending_queue_size = 3;
  auto obs_reader = node->CreateReader<Chatter>(obs_cfg, nullptr).value;
  if (!obs_reader) return 20;

  auto writer2 = node->CreateWriter<Chatter>("channel/observe").value;
  if (!writer2) return 21;
  SleepMs(500);

  for (int i = 1; i <= 5; ++i) {
    Chatter m;
    m.set_seq(i);
    if (!writer2->Write(m)) return 22;
    SleepMs(50);
  }
  SleepMs(kSettle.count());

  // 深度 3：应只保留 seq 3/4/5
  auto all = obs_reader->GetAllObserved();
  if (all.size() != 3) return 23;
  if (all.front()->seq() != 3) return 24;
  if (all.back()->seq() != 5) return 25;
  if (obs_reader->GetLatestObserved() == nullptr) return 26;
  if (obs_reader->GetLatestObserved()->seq() != 5) return 27;
  if (obs_reader->GetOldestObserved()->seq() != 3) return 28;
  if (obs_reader->GetHistoryDepth() != 3) return 29;

  // 动态调深/调浅
  obs_reader->SetHistoryDepth(2);
  if (obs_reader->GetHistoryDepth() != 2) return 30;
  all = obs_reader->GetAllObserved();
  if (all.size() != 2 || all.back()->seq() != 5) return 31;

  obs_reader->ClearData();
  if (!obs_reader->Empty()) return 32;

  // ---------- DeleteReader / GetReader ----------
  if (node->GetReader<Chatter>("channel/callback") == nullptr) return 40;
  if (node->GetReader<Chatter>("channel/nonexistent") != nullptr) return 41;
  if (!node->DeleteReader("channel/callback")) return 42;
  if (node->DeleteReader("channel/callback")) return 43;  // 重复删除失败
  if (node->GetReader<Chatter>("channel/callback") != nullptr) return 44;

  // ---------- 同节点同通道重复创建 Reader 应失败 ----------
  auto dup = node->CreateReader<Chatter>(
      "channel/observe", [](const std::shared_ptr<Chatter>&) {}).value;
  if (dup) return 50;

  firefly::Clear();
  return 0;
}
