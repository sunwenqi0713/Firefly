// clear_wait_test.cpp：慢回调期间 Reader::Shutdown 与全局 Clear 等待在途回调。
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

#include "firefly/firefly.h"
#include "test_messages.pb.h"

using firefly::CallbackResult;
using firefly::test::Chatter;

namespace {
constexpr auto kCallbackSleep = std::chrono::milliseconds(400);

bool WaitUntil(const std::atomic<bool>& flag, int max_ms = 3000) {
  for (int i = 0; i < max_ms / 10; ++i) {
    if (flag.load()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

std::shared_ptr<Chatter> Msg() {
  auto msg = std::make_shared<Chatter>();
  msg->set_seq(1);
  return msg;
}
}  // namespace

int main() {
  firefly::Init("clear_wait_test");
  auto node = firefly::CreateNode("clear_wait_node").value;
  if (!node) return 1;

  // 1) Reader::Shutdown 阻塞至在途回调返回
  std::atomic<bool> entered1{false};
  std::atomic<bool> done1{false};
  auto reader1 = node->CreateReader<Chatter>(
      "slow_ch_1",
      [&](const firefly::MessageContext&,
          const std::shared_ptr<const Chatter>&) {
        entered1.store(true);
        std::this_thread::sleep_for(kCallbackSleep);
        done1.store(true);
        return CallbackResult::kOk;
      }).value;
  auto writer1 = node->CreateWriter<Chatter>("slow_ch_1").value;
  if (!reader1 || !writer1) return 2;
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  writer1->Write(Msg());
  if (!WaitUntil(entered1)) return 3;

  auto t0 = std::chrono::steady_clock::now();
  reader1->Shutdown();
  auto elapsed1 = std::chrono::steady_clock::now() - t0;
  if (!done1.load()) return 4;
  if (elapsed1 < std::chrono::milliseconds(300)) return 5;

  // 2) 全局 Clear 等待调用时刻在途的回调返回
  std::atomic<bool> entered2{false};
  std::atomic<bool> done2{false};
  auto reader2 = node->CreateReader<Chatter>(
      "slow_ch_2",
      [&](const firefly::MessageContext&,
          const std::shared_ptr<const Chatter>&) {
        entered2.store(true);
        std::this_thread::sleep_for(kCallbackSleep);
        done2.store(true);
        return CallbackResult::kOk;
      }).value;
  auto writer2 = node->CreateWriter<Chatter>("slow_ch_2").value;
  if (!reader2 || !writer2) return 10;
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  writer2->Write(Msg());
  if (!WaitUntil(entered2)) return 11;

  auto t1 = std::chrono::steady_clock::now();
  firefly::Clear();
  auto elapsed2 = std::chrono::steady_clock::now() - t1;
  if (!done2.load()) return 12;
  if (elapsed2 < std::chrono::milliseconds(300)) return 13;

  return 0;
}
