// 使用示例：发布 / 订阅（Writer / Reader）
//
// 演示：
//   1. 回调模式订阅（消息到达即调用回调）
//   2. Observe 模式订阅（消息缓存，按需拉取）
//   3. HasWriter / HasReader 在线关系查询
//   4. 缓存深度控制 SetHistoryDepth / ClearData
//
// 消息类型使用 tests/test_messages.proto 生成的 firefly::test::Chatter。

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

#include "firefly/firefly.h"
#include "test_messages.pb.h"

using firefly::test::Chatter;

static void SleepMs(int ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

int main() {
  firefly::Init("pubsub_chat_example");
  auto pub_node = firefly::CreateNode("publisher");
  auto sub_node = firefly::CreateNode("subscriber");

  // ---- 回调模式订阅 ----
  std::atomic<int> cb_count{0};
  auto cb_reader = sub_node->CreateReader<Chatter>(
      "channel/chat",
      [&cb_count](const std::shared_ptr<Chatter>& msg) {
        ++cb_count;
        std::printf("[callback] seq=%lld content=%s\n",
                    static_cast<long long>(msg->seq()),
                    msg->content().c_str());
      }).value;
  if (!cb_reader) return 10;

  // ---- Observe 模式订阅（缓存 3 条）----
  // 同一节点不允许对同一通道重复订阅，故使用第二个订阅者节点。
  auto sub_node2 = firefly::CreateNode("subscriber2");
  firefly::ReaderConfig obs_cfg;
  obs_cfg.channel_name = "channel/chat";
  obs_cfg.pending_queue_size = 3;
  auto obs_reader = sub_node2->CreateReader<Chatter>(obs_cfg, nullptr).value;
  if (!obs_reader) return 11;

  // ---- 发布者 ----
  auto writer = pub_node->CreateWriter<Chatter>("channel/chat").value;
  if (!writer) return 12;

  SleepMs(500);  // 等待订阅者完成拨号
  std::printf("reader->HasWriter() = %d, writer->HasReader() = %d\n",
              cb_reader->HasWriter() ? 1 : 0, writer->HasReader() ? 1 : 0);

  for (int i = 1; i <= 5; ++i) {
    Chatter msg;
    msg.set_seq(i);
    msg.set_content("hello #" + std::to_string(i));
    if (!writer->Write(msg)) return 13;
    SleepMs(50);
  }
  SleepMs(500);  // 等待消息投递

  std::printf("callback received = %d\n", cb_count.load());

  // ---- Observe 拉取：缓存 3 条，应保留 seq 3/4/5 ----
  auto all = obs_reader->GetAllObserved();
  std::printf("observed count = %zu, latest seq = %lld\n", all.size(),
              all.empty() ? -1LL
                          : static_cast<long long>(all.back()->seq()));
  if (all.size() != 3) return 14;

  obs_reader->ClearData();
  std::printf("after ClearData: empty = %d\n", obs_reader->Empty() ? 1 : 0);
  if (!obs_reader->Empty()) return 15;

  firefly::Clear();
  return 0;
}
