// duplicate_test.cpp：CreateNode 进程内查重 + 单 Node 内 Writer/Service/Client 查重。
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "firefly/firefly.h"
#include "test_messages.pb.h"

using namespace std::chrono_literals;
using firefly::ErrorCode;
using firefly::test::CalcRequest;
using firefly::test::CalcResponse;
using firefly::test::Chatter;

static int Check(bool cond, int line) { return cond ? 0 : line; }

// 并发创建同名端点：查重与落表必须在同一临界区内完成，只有一个线程能成功。
// 回归点是旧实现「先解锁查重、初始化后再落表」的窗口——8 个线程会全部成功，
// 弱引用表只剩最后一个，DeleteWriter 再也定位不到其余实例。
static int TestConcurrentDuplicate() {
  auto node = firefly::CreateNode("dup_race_node").value;
  if (!node) return 60;

  constexpr int kThreads = 8;
  std::atomic<int> go{0};
  std::atomic<int> created{0};
  std::atomic<int> duplicate{0};
  std::atomic<int> failed{0};
  std::mutex survivor_mutex;
  std::shared_ptr<firefly::Writer<Chatter>> survivor;

  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&] {
      while (go.load() == 0) std::this_thread::sleep_for(1ms);  // 尽量同时起跑
      auto r = node->CreateWriter<Chatter>("dup_race_ch");
      if (static_cast<bool>(r)) {
        created.fetch_add(1);
        std::lock_guard<std::mutex> lg(survivor_mutex);
        survivor = r.value;
      } else if (r.code == ErrorCode::kDuplicate) {
        duplicate.fetch_add(1);
      } else {
        failed.fetch_add(1);
      }
    });
  }
  go.store(1);
  for (auto& t : threads) t.join();

  if (int e = Check(created.load() == 1, 61)) return e;
  if (int e = Check(duplicate.load() == kThreads - 1, 62)) return e;
  if (int e = Check(failed.load() == 0, 63)) return e;
  // 唯一成功者仍是表内可定位的实例：重复创建继续返回 kDuplicate。
  {
    auto again = node->CreateWriter<Chatter>("dup_race_ch");
    if (int e = Check(!again && again.code == ErrorCode::kDuplicate, 64)) {
      return e;
    }
  }
  // 释放后名字回收，可重新创建。
  survivor.reset();
  {
    auto fresh = node->CreateWriter<Chatter>("dup_race_ch");
    if (int e = Check(static_cast<bool>(fresh), 65)) return e;
    fresh.value->Shutdown();
  }
  return 0;
}

int main() {
  firefly::Init("duplicate_test");

  // CreateNode 进程内查重
  {
    auto n1 = firefly::CreateNode("dup_node");
    if (int e = Check(static_cast<bool>(n1), 10)) return e;
    auto n2 = firefly::CreateNode("dup_node");
    if (int e = Check(!n2 && n2.code == ErrorCode::kDuplicate &&
                          !n2.message.empty(), 11)) return e;
    // 非法名
    auto n3 = firefly::CreateNode("");
    if (int e = Check(!n3 && n3.code == ErrorCode::kInvalidName, 12)) return e;
    // n1 析构后名字回收，可重建
  }
  auto n_re = firefly::CreateNode("dup_node");
  if (int e = Check(static_cast<bool>(n_re), 13)) return e;

  auto node = firefly::CreateNode("dup_endpoint_node").value;

  // Writer 单 Node 内查重
  auto w1 = node->CreateWriter<Chatter>("dup_ch");
  if (int e = Check(static_cast<bool>(w1), 20)) return e;
  auto w2 = node->CreateWriter<Chatter>("dup_ch");
  if (int e = Check(!w2 && w2.code == ErrorCode::kDuplicate &&
                        w2.message.find("dup_ch") != std::string::npos, 21)) return e;
  // 不同通道名不冲突
  auto w3 = node->CreateWriter<Chatter>("dup_ch_other");
  if (int e = Check(static_cast<bool>(w3), 22)) return e;

  // Service 单 Node 内查重
  auto cb = [](const std::shared_ptr<const CalcRequest>&)
      -> firefly::ServiceResult<CalcResponse> {
    auto r = std::make_shared<CalcResponse>();
    r->set_sum(1);
    return {ErrorCode::kOk, r};
  };
  auto s1 = node->CreateService<CalcRequest, CalcResponse>("dup_svc", cb);
  if (int e = Check(static_cast<bool>(s1), 30)) return e;
  auto s2 = node->CreateService<CalcRequest, CalcResponse>("dup_svc", cb);
  if (int e = Check(!s2 && s2.code == ErrorCode::kDuplicate, 31)) return e;

  // Client 单 Node 内查重
  auto c1 = node->CreateClient<CalcRequest, CalcResponse>("dup_svc");
  if (int e = Check(static_cast<bool>(c1), 40)) return e;
  auto c2 = node->CreateClient<CalcRequest, CalcResponse>("dup_svc");
  if (int e = Check(!c2 && c2.code == ErrorCode::kDuplicate, 41)) return e;

  // Reader 现有约束回归：同节点同通道重复
  auto r1 = node->CreateReader<Chatter>("dup_read_ch");
  if (int e = Check(static_cast<bool>(r1), 50)) return e;
  auto r2 = node->CreateReader<Chatter>("dup_read_ch");
  if (int e = Check(!r2 && r2.code == ErrorCode::kDuplicate, 51)) return e;

  if (int e = TestConcurrentDuplicate()) return e;

  firefly::Clear();
  return 0;
}
