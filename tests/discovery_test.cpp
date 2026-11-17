// discovery 注册中心单元测试：验证主实例的注册/查询/订阅派发逻辑。
#include "firefly/detail/discovery.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "firefly/common/types.h"

using firefly::discovery::Registry;
using firefly::discovery::Kind;

int main() {
  Registry& reg = Registry::instance();
  if (!reg.Start()) return 1;

  // 注册 + 列表查询
  if (!reg.Register(Kind::kWriter, "chatter", "tcp://127.0.0.1:40001")) return 2;
  if (!reg.Register(Kind::kWriter, "chatter", "tcp://127.0.0.1:40002")) return 3;
  if (!reg.Has(Kind::kWriter, "chatter")) return 4;
  if (reg.Has(Kind::kReader, "chatter")) return 5;
  auto urls = reg.List(Kind::kWriter, "chatter");
  if (urls.size() != 2) return 6;

  // 订阅 + 通知(本地, 异步经 REP 线程)
  std::atomic<int> got{0};
  int id = reg.Subscribe(Kind::kWriter, "chatter",
                         [&](const firefly::discovery::Event& e) {
                           if (e.added) ++got;
                         });
  if (id == 0) return 7;

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  reg.Register(Kind::kWriter, "chatter", "tcp://127.0.0.1:40003");
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  if (got.load() != 1) return 8;

  // 注销
  reg.Unregister(Kind::kWriter, "chatter", "tcp://127.0.0.1:40003");
  urls = reg.List(Kind::kWriter, "chatter");
  if (urls.size() != 2) return 9;

  reg.Stop();
  return 0;
}