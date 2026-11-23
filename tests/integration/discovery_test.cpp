// discovery 注册中心单元测试：记录模型 / 多记录查询 / Watch 快照 / 按 id 注销。
#include "backends/nng/registry.h"

#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <vector>

#include "firefly/common/types.h"

using firefly::discovery::Registry;
using firefly::discovery::Kind;
using firefly::detail::EndpointRecord;

namespace {
EndpointRecord Make(Kind kind, const char* name, const char* address) {
  EndpointRecord rec;
  rec.kind = kind;
  rec.name = name;
  rec.address = address;
  return rec;
}
}  // namespace

int main() {
  Registry& reg = Registry::instance();
  if (!reg.Start()) return 1;

  const auto id1 = reg.Register(Make(Kind::kWriter, "chatter", "tcp://127.0.0.1:40001"));
  const auto id2 = reg.Register(Make(Kind::kWriter, "chatter", "tcp://127.0.0.1:40002"));
  if (id1 == 0 || id2 == 0 || id1 == id2) return 2;
  if (!reg.Has(Kind::kWriter, "chatter")) return 3;
  if (reg.Has(Kind::kReader, "chatter")) return 4;
  if (reg.Query(Kind::kWriter, "chatter").size() != 2) return 5;

  std::promise<std::size_t> initial;
  std::promise<std::size_t> updated;
  auto initial_future = initial.get_future();
  auto updated_future = updated.get_future();
  std::atomic<bool> initial_set{false};
  std::atomic<bool> updated_set{false};
  const int watch = reg.Watch(
      Kind::kWriter, "chatter", [&](const std::vector<EndpointRecord>& snapshot) {
        if (snapshot.size() == 2 && !initial_set.exchange(true)) {
          initial.set_value(snapshot.size());
        } else if (snapshot.size() == 3 && !updated_set.exchange(true)) {
          updated.set_value(snapshot.size());
        }
      });
  if (watch == 0) return 6;
  if (initial_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) return 7;
  if (initial_future.get() != 2) return 8;

  const auto id3 = reg.Register(Make(Kind::kWriter, "chatter", "tcp://127.0.0.1:40003"));
  if (id3 == 0) return 9;
  if (updated_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) return 10;
  if (updated_future.get() != 3) return 11;

  if (!reg.Unregister(id3)) return 12;
  if (reg.Query(Kind::kWriter, "chatter").size() != 2) return 13;
  if (!reg.Unregister(id3)) return 14;  // 重复注销 no-op
  reg.Unwatch(watch);

  // Clear 后的迟到注销必须是 no-op；重启后不得看到旧记录。
  reg.Clear();
  if (!reg.Unregister(id1)) return 15;
  if (!reg.Start()) return 16;
  if (!reg.Query(Kind::kWriter, "chatter").empty()) return 17;
  reg.Stop();
  return 0;
}
