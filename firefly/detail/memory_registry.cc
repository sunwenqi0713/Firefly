#include "firefly/detail/backend_access.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "firefly/common/log.h"

namespace firefly {
namespace detail {
namespace {

struct WatchEntry {
  int id;
  RegistryKind kind;
  std::string name;
  WatchCallback cb;
};

std::string Key(RegistryKind kind, const std::string& name) {
  switch (kind) {
    case RegistryKind::kWriter:
      return "writer\t" + name;
    case RegistryKind::kReader:
      return "reader\t" + name;
    case RegistryKind::kService:
      return "service\t" + name;
  }
  return "unknown\t" + name;
}

class MemoryRegistry final : public IRegistry {
 public:
  EndpointRecordId Register(const EndpointRecord& in) override {
    EndpointRecord rec = in;
    std::vector<WatchCallback> callbacks;
    std::vector<EndpointRecord> snapshot;
    {
      std::lock_guard<std::mutex> dispatch(dispatch_mutex_);
      {
        std::lock_guard<std::mutex> data(data_mutex_);
        if (rec.id == 0 || records_.count(rec.id) != 0) {
          rec.id = next_id_++;
        }
        if (rec.backend.empty()) rec.backend = "memory";
        records_[rec.id] = rec;
        by_key_[Key(rec.kind, rec.name)].insert(rec.id);
        snapshot = SnapshotLocked(rec.kind, rec.name);
      }
      callbacks = WatchersLocked(rec.kind, rec.name);
      Dispatch(callbacks, snapshot);
    }
    return rec.id;
  }

  bool Unregister(EndpointRecordId id) override {
    if (id == 0) return true;
    std::vector<WatchCallback> callbacks;
    std::vector<EndpointRecord> snapshot;
    RegistryKind kind = RegistryKind::kWriter;
    std::string name;
    {
      std::lock_guard<std::mutex> dispatch(dispatch_mutex_);
      {
        std::lock_guard<std::mutex> data(data_mutex_);
        auto it = records_.find(id);
        if (it == records_.end()) return true;  // Clear 后迟到注销 no-op
        kind = it->second.kind;
        name = it->second.name;
        records_.erase(it);
        auto kit = by_key_.find(Key(kind, name));
        if (kit != by_key_.end()) {
          kit->second.erase(id);
          if (kit->second.empty()) by_key_.erase(kit);
        }
        snapshot = SnapshotLocked(kind, name);
      }
      callbacks = WatchersLocked(kind, name);
    }
    Dispatch(callbacks, snapshot);
    return true;
  }

  std::vector<EndpointRecord> Query(RegistryKind kind, const std::string& name,
                                    const std::string& backend) override {
    std::lock_guard<std::mutex> data(data_mutex_);
    auto all = SnapshotLocked(kind, name);
    if (backend.empty()) return all;
    std::vector<EndpointRecord> out;
    for (const auto& rec : all) {
      if (rec.backend == backend) out.push_back(rec);
    }
    return out;
  }

  bool Has(RegistryKind kind, const std::string& name) override {
    std::lock_guard<std::mutex> data(data_mutex_);
    return !SnapshotLocked(kind, name).empty();
  }

  int Watch(RegistryKind kind, const std::string& name,
            WatchCallback&& cb) override {
    std::vector<EndpointRecord> snapshot;
    int watch_id = 0;
    {
      std::lock_guard<std::mutex> dispatch(dispatch_mutex_);
      {
        std::lock_guard<std::mutex> sub(sub_mutex_);
        watch_id = next_watch_id_++;
        watches_.push_back(WatchEntry{watch_id, kind, name, std::move(cb)});
      }
      {
        std::lock_guard<std::mutex> data(data_mutex_);
        snapshot = SnapshotLocked(kind, name);
      }
      Dispatch({watches_.back().cb}, snapshot);
    }
    return watch_id;
  }

  void Unwatch(int id) override {
    std::lock_guard<std::mutex> dispatch(dispatch_mutex_);
    std::lock_guard<std::mutex> sub(sub_mutex_);
    watches_.erase(std::remove_if(watches_.begin(), watches_.end(),
                                  [id](const WatchEntry& w) { return w.id == id; }),
                   watches_.end());
  }

  void Clear() override {
    std::lock_guard<std::mutex> dispatch(dispatch_mutex_);
    {
      std::lock_guard<std::mutex> data(data_mutex_);
      records_.clear();
      by_key_.clear();
    }
    {
      std::lock_guard<std::mutex> sub(sub_mutex_);
      watches_.clear();
    }
  }

 private:
  std::vector<EndpointRecord> SnapshotLocked(RegistryKind kind,
                                             const std::string& name) const {
    std::vector<EndpointRecord> out;
    auto it = by_key_.find(Key(kind, name));
    if (it == by_key_.end()) return out;
    for (EndpointRecordId id : it->second) {
      auto rec = records_.find(id);
      if (rec != records_.end()) out.push_back(rec->second);
    }
    return out;
  }

  std::vector<WatchCallback> WatchersLocked(RegistryKind kind,
                                            const std::string& name) const {
    std::vector<WatchCallback> out;
    for (const auto& w : watches_) {
      if (w.kind == kind && w.name == name && w.cb) out.push_back(w.cb);
    }
    return out;
  }

  static void Dispatch(const std::vector<WatchCallback>& callbacks,
                       const std::vector<EndpointRecord>& snapshot) {
    for (const auto& cb : callbacks) {
      try {
        cb(snapshot);
      } catch (...) {
        FY_ERROR("memory registry: watch callback threw");
      }
    }
  }

  // 锁序恒为 dispatch_mutex_ -> data_mutex_/sub_mutex_；回调在 dispatch 锁内派发。
  std::mutex dispatch_mutex_;
  std::mutex data_mutex_;
  std::mutex sub_mutex_;
  std::map<EndpointRecordId, EndpointRecord> records_;
  std::map<std::string, std::set<EndpointRecordId>> by_key_;
  std::vector<WatchEntry> watches_;
  EndpointRecordId next_id_ = 1;
  int next_watch_id_ = 1;
};

}  // namespace

std::shared_ptr<IRegistry> MakeMemoryRegistry() {
  return std::make_shared<MemoryRegistry>();
}

}  // namespace detail
}  // namespace firefly
