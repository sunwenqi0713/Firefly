// callback_contract_test.cpp：回调异常日志 + 回调/Observe 互斥契约（阶段二任务5）。
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

#include "firefly/firefly.h"
#include "test_messages.pb.h"

using firefly::test::Chatter;

static int Check(bool cond, int line) { return cond ? 0 : line; }

static Chatter MakeMsg(int seq) {
  Chatter m;
  m.set_seq(seq);
  return m;
}

int main() {
  firefly::Init("callback_contract_test");
  auto node = firefly::CreateNode("callback_node").value;

  // 1. 回调内抛异常不崩溃，且记录带 channel 名的 ERROR 日志（捕获 stderr）。
  // 重定向 stderr 到临时文件以检查日志内容。
  const char* log_path = "callback_contract_stderr.log";
  std::atomic<int> delivered{0};
  {
    auto cb = [&delivered](const std::shared_ptr<Chatter>&) {
      ++delivered;
      throw std::runtime_error("boom-from-callback");
    };
    auto reader = node->CreateReader<Chatter>("cb_throw_ch", cb).value;
    auto writer = node->CreateWriter<Chatter>("cb_throw_ch").value;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));  // 等待拨号

    // MSVC 下 stderr 是宏不可赋值，用 freopen 重定向；结束后用 CON 恢复。
    std::fflush(stderr);
    bool redirected = (std::freopen(log_path, "w", stderr) != nullptr);
    writer->Write(MakeMsg(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));  // 等待回调执行
    if (redirected) {
      std::fflush(stderr);
#ifdef _WIN32
      std::freopen("CON", "w", stderr);
#else
      std::freopen("/dev/tty", "w", stderr);
#endif
    }

    if (int e = Check(delivered.load() >= 1, 10)) return e;  // 回调仍被派发
    // 进程未崩溃即证明异常被捕获；检查日志含 channel 名与 what()
    if (redirected) {
      FILE* f = std::fopen(log_path, "r");
      std::string content;
      if (f) {
        char buf[512];
        while (std::fgets(buf, sizeof(buf), f)) content += buf;
        std::fclose(f);
      }
      if (int e = Check(content.find("cb_throw_ch") != std::string::npos, 11)) return e;
      if (int e = Check(content.find("boom-from-callback") != std::string::npos, 12)) return e;
    }
  }

  // 2. 回调模式与 Observe 互斥：回调模式下 GetAllObserved() 恒空。
  {
    std::atomic<int> cb2{0};
    auto cb_reader = node->CreateReader<Chatter>(
        "cb_observe_ch", [&cb2](const std::shared_ptr<Chatter>&) { ++cb2; }).value;
    auto writer = node->CreateWriter<Chatter>("cb_observe_ch").value;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    for (int i = 1; i <= 3; ++i) {
      writer->Write(MakeMsg(i));
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    if (int e = Check(cb2.load() >= 1, 20)) return e;
    if (int e = Check(cb_reader->GetAllObserved().empty(), 21)) return e;
    if (int e = Check(cb_reader->Empty(), 22)) return e;
  }

  // 3. Observe 模式（传 nullptr）：回调不触发，数据入队可拉取。
  {
    firefly::ReaderConfig cfg;
    cfg.channel_name = "observe_only_ch";
    cfg.pending_queue_size = 5;
    auto obs_reader = node->CreateReader<Chatter>(cfg, nullptr).value;
    auto writer = node->CreateWriter<Chatter>("observe_only_ch").value;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    writer->Write(MakeMsg(42));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    auto all = obs_reader->GetAllObserved();
    if (int e = Check(all.size() >= 1, 30)) return e;
    if (int e = Check(all.back()->seq() == 42, 31)) return e;
  }

  firefly::Clear();
  return 0;
}
