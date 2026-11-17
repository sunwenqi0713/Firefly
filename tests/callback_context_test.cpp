// callback_context_test.cpp：MessageContext 序号/时间/kDropped/异常日志契约。
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "firefly/firefly.h"
#include "test_messages.pb.h"

using firefly::CallbackResult;
using firefly::MessageContext;
using firefly::test::Chatter;

static std::shared_ptr<Chatter> Msg(long long seq) {
  auto msg = std::make_shared<Chatter>();
  msg->set_seq(seq);
  msg->set_content("ctx");
  return msg;
}

int main() {
  firefly::Init("callback_context_test");
  auto node = firefly::CreateNode("callback_context_node").value;
  if (!node) return 1;

  // 1) sequence 从 1 递增、channel 正确、receive_time 单调不倒退
  std::vector<std::uint64_t> sequences;
  std::vector<std::chrono::steady_clock::time_point> times;
  std::mutex mu;
  auto reader = node->CreateReader<Chatter>(
      "cb_ctx_ch",
      [&](const MessageContext& ctx, const std::shared_ptr<const Chatter>& msg) {
        std::lock_guard<std::mutex> lock(mu);
        sequences.push_back(ctx.sequence);
        times.push_back(ctx.receive_time);
        if (ctx.channel != "cb_ctx_ch") return CallbackResult::kDropped;
        if (msg->seq() < 0) return CallbackResult::kDropped;
        return CallbackResult::kOk;
      }).value;
  if (!reader) return 2;
  auto writer = node->CreateWriter<Chatter>("cb_ctx_ch").value;
  if (!writer) return 3;
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  for (long long i = 1; i <= 3; ++i) {
    if (writer->Write(Msg(i)) != firefly::WriteStatus::kAccepted) return 4;
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  {
    std::lock_guard<std::mutex> lock(mu);
    if (sequences.size() != 3) return 5;
    if (sequences[0] != 1 || sequences[1] != 2 || sequences[2] != 3) return 6;
    if (times[1] < times[0] || times[2] < times[1]) return 7;
  }

  // 2) kDropped 计入 Stats().dropped
  auto drop_reader = node->CreateReader<Chatter>(
      "cb_drop_ch",
      [](const MessageContext&, const std::shared_ptr<const Chatter>&) {
        return CallbackResult::kDropped;
      }).value;
  if (!drop_reader) return 10;
  auto drop_writer = node->CreateWriter<Chatter>("cb_drop_ch").value;
  if (!drop_writer) return 11;
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  drop_writer->Write(Msg(1));
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  if (drop_reader->Stats().dropped != 1) return 12;

  // 3) 异常回调不崩溃，日志带 what()（用可替换 sink 捕获，不再重定向 stderr）
  std::string captured;
  firefly::SetLogSink([&](firefly::LogLevel level, const char* message) {
    if (level == firefly::LogLevel::kError) captured += message;
  });
  {
    auto bad_reader = node->CreateReader<Chatter>(
        "cb_ex_ch",
        [](const MessageContext&, const std::shared_ptr<const Chatter>&)
            -> CallbackResult {
          throw std::runtime_error("boom-in-context-test");
        }).value;
    auto bad_writer = node->CreateWriter<Chatter>("cb_ex_ch").value;
    if (!bad_reader || !bad_writer) return 20;
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    bad_writer->Write(Msg(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
  }
  firefly::SetLogSink(nullptr);
  if (captured.find("boom-in-context-test") == std::string::npos) return 21;
  if (captured.find("cb_ex_ch") == std::string::npos) return 22;

  firefly::Clear();
  return 0;
}
