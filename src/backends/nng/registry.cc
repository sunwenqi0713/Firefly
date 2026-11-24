#include "backends/nng/registry.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nng/nng.h>
#include <nng/protocol/pubsub0/pub.h>
#include <nng/protocol/pubsub0/sub.h>
#include <nng/protocol/reqrep0/rep.h>
#include <nng/protocol/reqrep0/req.h>

#include "firefly/common/log.h"
#include "core/runtime.h"

namespace firefly {
namespace discovery {

const char* KindToString(Kind k) {
  switch (k) {
    case Kind::kWriter:
      return "writer";
    case Kind::kReader:
      return "reader";
    case Kind::kService:
      return "service";
  }
  return "unknown";
}

namespace {

constexpr char kDefaultRepUrl[] = "tcp://127.0.0.1:37221";
constexpr char kDefaultPubUrl[] = "tcp://127.0.0.1:37222";
constexpr char kDefaultBackend[] = "nng";
constexpr int kPollTimeoutMs = 200;
constexpr int kReqTimeoutMs = 500;
// Follower 定期与主实例对账：弥补启动时错过的广播、SUB 重连期间的漏报。
constexpr auto kReconcileInterval = std::chrono::seconds(3);

// 发现地址在 Start 时读取；Configure 必须在首次 Start 前调用。
std::mutex g_config_mutex;
std::string g_rep_url = kDefaultRepUrl;
std::string g_pub_url = kDefaultPubUrl;

struct WatchEntry {
  int id;
  Kind kind;
  std::string name;
  WatchCallback cb;
};

std::string Key(Kind kind, const std::string& name) {
  return std::string(KindToString(kind)) + "\t" + name;
}

// 旧版（无 id 字段）记录派生稳定兼容 id：最高位为 1，与本地分配的 id 不冲突。
EndpointRecordId LegacyId(Kind kind, const std::string& name,
                          const std::string& address) {
  const std::string text = Key(kind, name) + "\t" + address;
  std::uint64_t hash = 1469598103934665603ULL;
  for (unsigned char c : text) {
    hash ^= c;
    hash *= 1099511628211ULL;
  }
  return (std::uint64_t{1} << 63) | (hash & 0x7fffffffffffffffULL);
}

bool KindFromString(const std::string& s, Kind* out) {
  if (s == "writer") {
    *out = Kind::kWriter;
  } else if (s == "reader") {
    *out = Kind::kReader;
  } else if (s == "service") {
    *out = Kind::kService;
  } else {
    return false;
  }
  return true;
}

void CloseSocket(nng_socket& s) {
  if (s.id != 0) {
    nng_close(s);
    s.id = 0;
  }
}

std::string RecvToString(nng_socket s) {
  void* data = nullptr;
  size_t size = 0;
  int r = nng_recv(s, &data, &size, NNG_FLAG_ALLOC);
  if (r != 0) {
    if (r != NNG_ETIMEDOUT && r != NNG_ECLOSED) {
      FY_WARN("nng_recv: %s", nng_strerror(r));
    }
    return "";
  }
  std::string out(static_cast<char*>(data), size);
  nng_free(data, size);
  return out;
}

bool SendRaw(nng_socket s, const std::string& msg) {
  int r = nng_send(s, const_cast<char*>(msg.data()), msg.size(), 0);
  if (r != 0) {
    FY_WARN("nng_send: %s", nng_strerror(r));
    return false;
  }
  return true;
}

}  // namespace

// 嵌套类定义（私有，仅在实现中使用）
struct Registry::Impl {
  bool primary = false;
  std::atomic<bool> running_{false};
  std::mutex start_mutex_;

  nng_socket rep_sock_{0};
  nng_socket pub_sock_{0};
  nng_socket req_sock_{0};
  nng_socket sub_sock_{0};

  // 锁序恒为 dispatch_mutex_ → data_mutex_ → sub_mutex_。回调在持有
  // dispatch_mutex_ 时派发，保证 Watch 的初始快照与后续变更顺序一致。
  std::mutex dispatch_mutex_;
  std::mutex data_mutex_;
  std::map<EndpointRecordId, EndpointRecord> records_;
  std::map<std::string, std::set<EndpointRecordId>> by_key_;
  // 修订号（均受 data_mutex_ 保护）：id_revision_ 记录某个 id 最近一次本地
  // 变更；removed_revision_ 记录注销广播（即便本地原本没有该记录），用于
  // 判断远端快照是否陈旧，避免"快照覆盖其后到达的增删"。
  std::uint64_t revision_ = 0;
  std::map<EndpointRecordId, std::uint64_t> id_revision_;
  std::map<EndpointRecordId, std::uint64_t> removed_revision_;
  const std::uint32_t process_tag_ = static_cast<std::uint32_t>(
      std::chrono::steady_clock::now().time_since_epoch().count() &
      0x7fffffffULL);
  std::uint32_t next_local_id_ = 1;

  std::mutex sub_mutex_;
  std::vector<WatchEntry> watches_;
  int next_watch_id_ = 1;

  std::string rep_url;
  std::string pub_url;

  std::mutex req_mutex_;

  std::thread rep_thread_;
  std::thread sub_thread_;
};

namespace {

EndpointRecordId MakeLocalId(Registry::Impl& impl) {
  return (static_cast<EndpointRecordId>(impl.process_tag_) << 32) |
         (impl.next_local_id_++ & 0xffffffffULL);
}

std::vector<EndpointRecord> SnapshotLocked(Registry::Impl& impl, Kind kind,
                                           const std::string& name) {
  std::vector<EndpointRecord> out;
  const auto it = impl.by_key_.find(Key(kind, name));
  if (it == impl.by_key_.end()) return out;
  for (EndpointRecordId id : it->second) {
    const auto rec = impl.records_.find(id);
    if (rec != impl.records_.end()) out.push_back(rec->second);
  }
  return out;
}

// 调用前持有 dispatch_mutex_；本函数内部取得 data_mutex_ 与 sub_mutex_。
void DispatchLocked(Registry::Impl& impl, Kind kind, const std::string& name) {
  std::vector<WatchCallback> callbacks;
  std::vector<EndpointRecord> snapshot;
  {
    std::lock_guard<std::mutex> data(impl.data_mutex_);
    snapshot = SnapshotLocked(impl, kind, name);
  }
  {
    std::lock_guard<std::mutex> sub(impl.sub_mutex_);
    for (const auto& w : impl.watches_) {
      if (w.kind == kind && w.name == name && w.cb) callbacks.push_back(w.cb);
    }
  }
  for (const auto& cb : callbacks) {
    try {
      cb(snapshot);
    } catch (...) {
      FY_ERROR("registry: watch callback threw");
    }
  }
}

// 调用前持有 dispatch_mutex_ 与 data_mutex_。
EndpointRecordId ApplyLocked(Registry::Impl& impl, const EndpointRecord& in,
                             bool added) {
  if (added) {
    EndpointRecord rec = in;
    if (rec.id == 0 || impl.records_.count(rec.id) != 0) {
      rec.id = MakeLocalId(impl);
    }
    if (rec.backend.empty()) rec.backend = kDefaultBackend;
    const auto key = Key(rec.kind, rec.name);
    impl.records_[rec.id] = rec;
    impl.by_key_[key].insert(rec.id);
    impl.removed_revision_.erase(rec.id);
    impl.id_revision_[rec.id] = ++impl.revision_;
    return rec.id;
  }
  EndpointRecordId id = in.id;
  auto it = impl.records_.find(id);
  if (it == impl.records_.end()) {
    // 兼容旧版 UNREG：按 kind+name+address 找回派生 id。
    for (const auto& kv : impl.records_) {
      if (kv.second.kind == in.kind && kv.second.name == in.name &&
          kv.second.address == in.address) {
        id = kv.first;
        it = impl.records_.find(id);
        break;
      }
    }
  }
  if (it == impl.records_.end()) return 0;
  impl.removed_revision_[id] = ++impl.revision_;
  const auto key = Key(it->second.kind, it->second.name);
  impl.records_.erase(it);
  auto kit = impl.by_key_.find(key);
  if (kit != impl.by_key_.end()) {
    kit->second.erase(id);
    if (kit->second.empty()) impl.by_key_.erase(kit);
  }
  return id;
}

std::string BroadcastNote(const EndpointRecord& rec, bool added) {
  std::ostringstream out;
  out << (added ? "A|" : "R|") << KindToString(rec.kind) << '|' << rec.name
      << '|' << rec.address << '|' << rec.id << '|' << rec.backend;
  return out.str();
}

void Broadcast(Registry::Impl& impl, const EndpointRecord& rec, bool added) {
  if (impl.pub_sock_.id == 0) return;
  SendRaw(impl.pub_sock_, BroadcastNote(rec, added));
}

std::string FormatRecords(const std::vector<EndpointRecord>& records) {
  std::ostringstream out;
  out << "OK " << records.size() << "\n";
  for (const auto& rec : records) {
    out << rec.id << '\t' << rec.backend << '\t' << rec.address << '\n';
  }
  return out.str();
}

// 主实例：执行一条命令并返回响应文本。响应在原协议末尾追加 id，旧版客户端
// 只读取第一行的 "OK"，忽略新增字段。
std::string Execute(Registry::Impl& impl, const std::string& req) {
  std::istringstream in(req);
  std::string cmd, kind, name, url, id_text, backend;
  std::getline(in, cmd, '\t');

  if (cmd == "REG" || cmd == "UNREG") {
    std::getline(in, kind, '\t');
    std::getline(in, name, '\t');
    std::getline(in, url, '\t');
    // 线协议版本探测：旧客户端只发 4 个字段（末尾无 \t），并严格比较
    // "OK\n"；新客户端固定追加 id/backend 两个字段（可为空），因此读完
    // url 后流尚未结束。对旧请求保留原响应，避免"注册失败但 Has() 为真"。
    const bool legacy_request = in.eof();
    std::getline(in, id_text, '\t');
    std::getline(in, backend, '\t');
    Kind k;
    if (!KindFromString(kind, &k)) return "ERR bad kind\n";
    if (name.empty()) return "ERR bad name\n";

    EndpointRecord rec;
    rec.id = id_text.empty() ? 0 : std::stoull(id_text);
    rec.kind = k;
    rec.name = name;
    rec.address = url;
    rec.backend = backend;
    EndpointRecordId id = 0;
    {
      std::lock_guard<std::mutex> dispatch(impl.dispatch_mutex_);
      {
        std::lock_guard<std::mutex> data(impl.data_mutex_);
        id = ApplyLocked(impl, rec, cmd == "REG");
      }
      if (id != 0) {
        rec.id = id;
        DispatchLocked(impl, k, name);
      }
    }
    if (id == 0) return "OK\n";
    Broadcast(impl, rec, cmd == "REG");
    // 旧版 Follower 只能识别 "OK\n"；新版客户端需要 id 做精确注销。
    return legacy_request ? "OK\n" : "OK " + std::to_string(id) + "\n";
  }
  if (cmd == "QLIST") {
    std::getline(in, kind, '\t');
    std::getline(in, name, '\t');
    std::getline(in, backend, '\t');
    Kind k;
    if (!KindFromString(kind, &k)) return "ERR bad kind\n";
    std::vector<EndpointRecord> records;
    {
      std::lock_guard<std::mutex> data(impl.data_mutex_);
      records = SnapshotLocked(impl, k, name);
    }
    if (!backend.empty()) {
      records.erase(std::remove_if(records.begin(), records.end(),
                                   [&backend](const EndpointRecord& r) {
                                     return r.backend != backend;
                                   }),
                    records.end());
    }
    return FormatRecords(records);
  }
  if (cmd == "LIST" || cmd == "HAS") {
    std::getline(in, kind, '\t');
    std::getline(in, name, '\t');
    Kind k;
    if (!KindFromString(kind, &k)) return "ERR bad kind\n";
    std::vector<EndpointRecord> records;
    {
      std::lock_guard<std::mutex> data(impl.data_mutex_);
      records = SnapshotLocked(impl, k, name);
    }
    bool has = !records.empty();
    std::vector<std::string> urls;
    for (const auto& rec : records) {
      if (!rec.address.empty()) urls.push_back(rec.address);
    }
    if (cmd == "HAS") return has ? "OK 1\n" : "OK 0\n";
    std::ostringstream out;
    out << "OK " << urls.size() << "\n";
    for (const auto& u : urls) out << u << '\n';
    return out.str();
  }
  return "ERR unknown cmd\n";
}

void RunRepThread(Registry::Impl& impl) {
  FY_INFO("registry: primary on %s", impl.rep_url.c_str());
  while (impl.running_.load() &&
         !runtime::ShutdownManager::instance().IsShutdown()) {
    std::string req = RecvToString(impl.rep_sock_);
    if (req.empty()) continue;
    std::string resp = Execute(impl, req);
    SendRaw(impl.rep_sock_, resp);
  }
  FY_INFO("registry: rep loop exit");
}

// 定义见本文件末尾：定期从主实例重新对账 Watch 的键。
void ReconcileWatches(Registry::Impl& impl);

void RunSubLoop(Registry::Impl& impl) {
  FY_INFO("registry: follower on %s", impl.rep_url.c_str());
  auto next_reconcile = std::chrono::steady_clock::now() + kReconcileInterval;
  while (impl.running_.load() &&
         !runtime::ShutdownManager::instance().IsShutdown()) {
    if (std::chrono::steady_clock::now() >= next_reconcile) {
      next_reconcile = std::chrono::steady_clock::now() + kReconcileInterval;
      ReconcileWatches(impl);
    }
    std::string line = RecvToString(impl.sub_sock_);
    if (line.empty()) continue;
    std::istringstream in(line);
    std::string tag, kind, name, address, id_text, backend;
    std::getline(in, tag, '|');
    std::getline(in, kind, '|');
    std::getline(in, name, '|');
    std::getline(in, address, '|');
    std::getline(in, id_text, '|');
    std::getline(in, backend, '|');
    Kind k;
    if (!KindFromString(kind, &k)) continue;
    EndpointRecord rec;
    rec.kind = k;
    rec.name = name;
    rec.address = address;
    rec.id = id_text.empty() ? LegacyId(k, name, address)
                             : std::stoull(id_text);
    rec.backend = backend.empty() ? kDefaultBackend : backend;
    const bool added = tag == "A";
    {
      std::lock_guard<std::mutex> dispatch(impl.dispatch_mutex_);
      bool changed = false;
      {
        std::lock_guard<std::mutex> data(impl.data_mutex_);
        const auto existing = impl.records_.find(rec.id);
        if (added) {
          // 本地 Register 已应用过同一条记录时，广播是重复通知。
          if (existing == impl.records_.end() ||
              existing->second.address != rec.address) {
            ApplyLocked(impl, rec, true);
            changed = true;
          }
        } else if (existing != impl.records_.end()) {
          ApplyLocked(impl, rec, false);
          changed = true;
        } else {
          // 本地没有该记录也要记注销修订号：对账查询可能拿到删除前的旧
          // 快照，随后合并时据此丢弃陈旧记录。
          impl.removed_revision_[rec.id] = ++impl.revision_;
        }
      }
      if (changed) DispatchLocked(impl, k, name);
    }
  }
  FY_INFO("registry: follower loop exit");
}

std::string RemoteRequest(Registry::Impl& impl, const std::string& line) {
  std::lock_guard<std::mutex> lock(impl.req_mutex_);
  if (impl.req_sock_.id == 0) return "";
  if (!SendRaw(impl.req_sock_, line)) return "";
  return RecvToString(impl.req_sock_);
}

// 解析 "OK <count>\n<id>\t<backend>\t<address>\n..."。返回 false 表示响应不是
// 一次成功查询（超时/空响应/ERR）：调用方据此区分"查询失败"与"查询成功但
// 结果为空"，后者是权威的空快照。
bool ParseRecords(const std::string& resp, Kind kind, const std::string& name,
                  std::vector<EndpointRecord>* out) {
  out->clear();
  std::istringstream in(resp);
  std::string status;
  std::size_t count = 0;
  if (!(in >> status >> count)) return false;
  if (status != "OK") return false;
  std::string line;
  std::getline(in, line);  // 吞掉行尾
  while (out->size() < count && std::getline(in, line)) {
    if (line.empty()) continue;
    std::istringstream fields(line);
    EndpointRecord rec;
    // 线协议只在首部携带 kind/name，记录行里没有；必须回填，否则合并后
    // 记录会落到空键下，Watch 快照依旧为空。
    rec.kind = kind;
    rec.name = name;
    std::string id_text;
    std::getline(fields, id_text, '\t');
    std::getline(fields, rec.backend, '\t');
    std::getline(fields, rec.address, '\t');
    rec.id = id_text.empty() ? 0 : std::stoull(id_text);
    out->push_back(std::move(rec));
  }
  return true;
}

// 旧主实例（或尚未升级的进程）不认识 QLIST：回退原 LIST 文本协议并派生
// 兼容字段。与 Registry::Query 的回退逻辑保持一致。
bool ParseLegacyList(const std::string& resp, Kind kind, const std::string& name,
                     const std::string& backend,
                     std::vector<EndpointRecord>* out) {
  out->clear();
  std::istringstream in(resp);
  std::string status;
  std::size_t count = 0;
  if (!(in >> status >> count)) return false;
  if (status != "OK") return false;
  std::string address;
  while (out->size() < count && std::getline(in, address, '\n')) {
    if (address.empty()) continue;
    EndpointRecord rec;
    rec.kind = kind;
    rec.name = name;
    rec.address = address;
    rec.backend = kDefaultBackend;
    rec.id = LegacyId(kind, name, address);
    if (backend.empty() || backend == rec.backend) out->push_back(rec);
  }
  return true;
}

// 从实例直接向主实例取快照：Watch 的初始集合与定期对账共用。
// 返回 false 表示查询失败（主实例不可达、超时或响应不可解析），此时 *out
// 不代表任何权威状态，调用方必须保留本地记录；返回 true 时 *out 是该键的
// 完整快照，可能为空——空集合意味着这些端点确实已不存在。
bool QueryRemote(Registry::Impl& impl, Kind kind, const std::string& name,
                 const std::string& backend,
                 std::vector<EndpointRecord>* out) {
  out->clear();
  std::ostringstream line;
  line << "QLIST\t" << KindToString(kind) << '\t' << name << '\t' << backend;
  if (ParseRecords(RemoteRequest(impl, line.str()), kind, name, out)) return true;
  // 旧主实例不识别 QLIST，会回 ERR；此处再走一次 LIST 文本协议。
  line.str(std::string());
  line.clear();
  line << "LIST\t" << KindToString(kind) << '\t' << name;
  const std::string resp = RemoteRequest(impl, line.str());
  if (resp.empty()) return false;
  return ParseLegacyList(resp, kind, name, backend, out);
}

// 发现键内的记录身份：地址 + 后端。同一 kind+name 下地址唯一标识一个端点，
// 反向对账据此判断"远端仍有这条记录"。无地址记录（Reader 占位）不能这样
// 认同——多个 Reader 共用空地址——只能按 id 精确匹配。
std::string DiscoveryKeyOf(const EndpointRecord& rec) {
  return rec.address + '\n' +
         (rec.backend.empty() ? kDefaultBackend : rec.backend);
}

// 远端权威快照的身份索引：id 集合用于精确匹配（无地址记录只能靠它），
// (地址, 后端) 集合用于兼容新旧协议下 id 不一致但地址相同的同一条登记。
struct RemoteIdentity {
  std::set<EndpointRecordId> ids;
  std::set<std::string> addresses;
};

bool ListedInRemote(const EndpointRecord& local, const RemoteIdentity& remote) {
  if (remote.ids.count(local.id) != 0) return true;
  if (local.address.empty()) return false;
  return remote.addresses.count(DiscoveryKeyOf(local)) != 0;
}

// 调用前持有 dispatch_mutex_ 与 data_mutex_。反向对账：本地存在、权威快照
// 里却没有的记录，说明注销广播丢了；只删除查询开始前未被本地更新的记录
// （id_revision_ <= query_revision），避免误删查询期间到达的新增或变更。
bool RemoveStaleLocked(Registry::Impl& impl, Kind kind,
                       const std::string& name,
                       const RemoteIdentity& remote,
                       std::uint64_t query_revision) {
  const auto index = impl.by_key_.find(Key(kind, name));
  if (index == impl.by_key_.end()) return false;
  std::vector<EndpointRecord> stale;
  for (EndpointRecordId id : index->second) {
    const auto rec = impl.records_.find(id);
    if (rec == impl.records_.end()) continue;
    const auto revised = impl.id_revision_.find(id);
    if (revised != impl.id_revision_.end() &&
        revised->second > query_revision) {
      continue;  // 查询开始后本地才新增/变更：以本地为准
    }
    if (ListedInRemote(rec->second, remote)) continue;
    stale.push_back(rec->second);
  }
  bool changed = false;
  for (const auto& rec : stale) {
    EndpointRecord victim = rec;  // ApplyLocked 会从表里擦除，传副本
    ApplyLocked(impl, victim, false);
    changed = true;
  }
  return changed;
}

// 调用前持有 dispatch_mutex_；内部取 data_mutex_。query_revision 是发起远端
// 查询之前的本地修订号：查询期间到达的广播（新增或注销）修订号更大，必须
// 优先于快照，否则晚启动 Reader 可能先拿到旧快照再被过期记录污染。
// 双向合并：补远端有、本地缺的记录，删远端无、本地仍留的记录。
bool MergeSnapshotLocked(Registry::Impl& impl, Kind kind,
                         const std::string& name,
                         const std::vector<EndpointRecord>& remote,
                         std::uint64_t query_revision) {
  bool changed = false;
  std::lock_guard<std::mutex> data(impl.data_mutex_);
  RemoteIdentity identity;
  for (const auto& rec : remote) {
    if (rec.id == 0) continue;
    // 远端仍列出该记录（哪怕本次因修订号更高而不采用），就算"在册"，
    // 反向对账不得删它。
    identity.ids.insert(rec.id);
    if (!rec.address.empty()) identity.addresses.insert(DiscoveryKeyOf(rec));
    const auto removed = impl.removed_revision_.find(rec.id);
    if (removed != impl.removed_revision_.end() &&
        removed->second > query_revision) {
      continue;
    }
    const auto changed_id = impl.id_revision_.find(rec.id);
    if (changed_id != impl.id_revision_.end() &&
        changed_id->second > query_revision) {
      continue;
    }
    const auto existing = impl.records_.find(rec.id);
    if (existing != impl.records_.end() &&
        existing->second.address == rec.address &&
        existing->second.backend == rec.backend) {
      continue;
    }
    ApplyLocked(impl, rec, true);
    changed = true;
  }
  if (RemoveStaleLocked(impl, kind, name, identity, query_revision)) {
    changed = true;
  }
  return changed;
}

// 定期重连/漏广播对账：只处理当前有 Watch 的键，且仅在合并确实改变记录
// 集合时派发回调，避免每轮无谓地触发 UpdatePublishers。查询失败（主实例
// 暂时不可达）不改变本地记录；查询成功但为空是权威结果，必须据此清空。
void ReconcileWatches(Registry::Impl& impl) {
  if (impl.primary || !impl.running_.load()) return;
  std::set<std::pair<Kind, std::string>> keys;
  {
    std::lock_guard<std::mutex> sub(impl.sub_mutex_);
    for (const auto& w : impl.watches_) keys.emplace(w.kind, w.name);
  }
  for (const auto& key : keys) {
    std::uint64_t start_revision = 0;
    {
      std::lock_guard<std::mutex> data(impl.data_mutex_);
      start_revision = impl.revision_;
    }
    std::vector<EndpointRecord> remote;
    if (!QueryRemote(impl, key.first, key.second, std::string(), &remote)) {
      continue;
    }
    std::lock_guard<std::mutex> dispatch(impl.dispatch_mutex_);
    if (MergeSnapshotLocked(impl, key.first, key.second, remote,
                            start_revision)) {
      DispatchLocked(impl, key.first, key.second);
    }
  }
}

}  // namespace anonymous

// ---------------- 公有接口 ----------------

Registry::Registry() : impl_(new Impl) {}
Registry::~Registry() { Stop(); }

Registry& Registry::instance() {
  static Registry registry;
  return registry;
}

void Registry::Configure(const std::string& rep_url, const std::string& pub_url) {
  std::lock_guard<std::mutex> lock(g_config_mutex);
  if (!rep_url.empty()) g_rep_url = rep_url;
  if (!pub_url.empty()) g_pub_url = pub_url;
}

bool Registry::Start() {
  std::lock_guard<std::mutex> lock(impl_->start_mutex_);
  if (impl_->running_.load()) return true;
  {
    std::lock_guard<std::mutex> cfg(g_config_mutex);
    impl_->rep_url = g_rep_url;
    impl_->pub_url = g_pub_url;
  }

  int r = nng_rep0_open(&impl_->rep_sock_);
  if (r != 0) {
    FY_ERROR("registry: nng_rep0_open: %s", nng_strerror(r));
    return false;
  }
  nng_setopt_ms(impl_->rep_sock_, NNG_OPT_RECVTIMEO, kPollTimeoutMs);
  r = nng_listen(impl_->rep_sock_, impl_->rep_url.c_str(), nullptr, 0);
  if (r == 0) {
    r = nng_pub0_open(&impl_->pub_sock_);
    if (r != 0) {
      FY_ERROR("registry: nng_pub0_open: %s", nng_strerror(r));
      CloseSocket(impl_->rep_sock_);
      return false;
    }
    r = nng_listen(impl_->pub_sock_, impl_->pub_url.c_str(), nullptr, 0);
    if (r != 0) {
      FY_ERROR("registry: pub listen %s: %s", impl_->pub_url.c_str(),
               nng_strerror(r));
      CloseSocket(impl_->rep_sock_);
      CloseSocket(impl_->pub_sock_);
      return false;
    }
    impl_->primary = true;
    impl_->rep_thread_ = std::thread(&RunRepThread, std::ref(*impl_));
    impl_->running_.store(true);
    FY_INFO("registry: acting as primary");
    return true;
  }

  if (r != NNG_EADDRINUSE) {
    FY_ERROR("registry: rep listen %s: %s", impl_->rep_url.c_str(),
             nng_strerror(r));
    CloseSocket(impl_->rep_sock_);
    return false;
  }
  CloseSocket(impl_->rep_sock_);

  r = nng_req0_open(&impl_->req_sock_);
  if (r != 0) {
    FY_ERROR("registry: nng_req0_open: %s", nng_strerror(r));
    return false;
  }
  nng_setopt_ms(impl_->req_sock_, NNG_OPT_SENDTIMEO, kReqTimeoutMs);
  nng_setopt_ms(impl_->req_sock_, NNG_OPT_RECVTIMEO, kReqTimeoutMs);
  r = nng_dial(impl_->req_sock_, impl_->rep_url.c_str(), nullptr, 0);
  if (r != 0) {
    FY_ERROR("registry: req dial %s: %s", impl_->rep_url.c_str(),
             nng_strerror(r));
    CloseSocket(impl_->req_sock_);
    return false;
  }

  r = nng_sub0_open(&impl_->sub_sock_);
  if (r != 0) {
    FY_ERROR("registry: nng_sub0_open: %s", nng_strerror(r));
    CloseSocket(impl_->req_sock_);
    return false;
  }
  nng_setopt(impl_->sub_sock_, NNG_OPT_SUB_SUBSCRIBE, "", 0);
  nng_setopt_ms(impl_->sub_sock_, NNG_OPT_RECVTIMEO, kPollTimeoutMs);
  r = nng_dial(impl_->sub_sock_, impl_->pub_url.c_str(), nullptr, 0);
  if (r != 0) {
    FY_ERROR("registry: sub dial %s: %s", impl_->pub_url.c_str(),
             nng_strerror(r));
    CloseSocket(impl_->req_sock_);
    CloseSocket(impl_->sub_sock_);
    return false;
  }

  impl_->primary = false;
  impl_->sub_thread_ = std::thread(&RunSubLoop, std::ref(*impl_));
  impl_->running_.store(true);
  FY_INFO("registry: acting as follower");
  return true;
}

void Registry::Stop() {
  if (!impl_->running_.load()) return;
  impl_->running_.store(false);
  if (impl_->rep_thread_.joinable() &&
      impl_->rep_thread_.get_id() != std::this_thread::get_id()) {
    impl_->rep_thread_.join();
  }
  if (impl_->sub_thread_.joinable() &&
      impl_->sub_thread_.get_id() != std::this_thread::get_id()) {
    impl_->sub_thread_.join();
  }
  CloseSocket(impl_->rep_sock_);
  CloseSocket(impl_->pub_sock_);
  CloseSocket(impl_->req_sock_);
  CloseSocket(impl_->sub_sock_);
  // Clear/重启语义：记录、索引与 Watch 订阅一起清空，迟到 Unregister 为 no-op。
  {
    std::lock_guard<std::mutex> dispatch(impl_->dispatch_mutex_);
    {
      std::lock_guard<std::mutex> data(impl_->data_mutex_);
      impl_->records_.clear();
      impl_->by_key_.clear();
      impl_->id_revision_.clear();
      impl_->removed_revision_.clear();
    }
    {
      std::lock_guard<std::mutex> sub(impl_->sub_mutex_);
      impl_->watches_.clear();
    }
  }
  impl_->primary = false;
}

EndpointRecordId Registry::Register(const EndpointRecord& record) {
  if (!Start()) return 0;
  std::ostringstream line;
  line << "REG\t" << KindToString(record.kind) << '\t' << record.name << '\t'
       << record.address << '\t' << record.id << '\t' << record.backend;
  EndpointRecordId id = record.id;
  std::string resp = impl_->primary ? Execute(*impl_, line.str())
                                    : RemoteRequest(*impl_, line.str());
  if (resp.rfind("OK ", 0) == 0) {
    try {
      id = std::stoull(resp.substr(3));
    } catch (...) {
      id = LegacyId(record.kind, record.name, record.address);
    }
  } else if (resp == "OK\n") {
    id = LegacyId(record.kind, record.name, record.address);
  } else {
    return 0;
  }
  // 从实例：请求返回后先把记录落到本地表，广播中的同一 id 会被识别为重复。
  if (!impl_->primary && id != 0) {
    EndpointRecord rec = record;
    rec.id = id;
    if (rec.backend.empty()) rec.backend = kDefaultBackend;
    {
      std::lock_guard<std::mutex> dispatch(impl_->dispatch_mutex_);
      bool changed = false;
      {
        std::lock_guard<std::mutex> data(impl_->data_mutex_);
        if (impl_->records_.count(id) == 0) {
          ApplyLocked(*impl_, rec, true);
          changed = true;
        }
      }
      if (changed) DispatchLocked(*impl_, rec.kind, rec.name);
    }
  }
  return id;
}

bool Registry::Unregister(EndpointRecordId id) {
  if (!impl_->running_.load()) return true;  // Clear 后的迟到注销
  if (id == 0) return true;
  EndpointRecord rec;
  {
    std::lock_guard<std::mutex> data(impl_->data_mutex_);
    const auto it = impl_->records_.find(id);
    if (it != impl_->records_.end()) rec = it->second;
  }
  std::ostringstream line;
  line << "UNREG\t" << KindToString(rec.kind) << '\t' << rec.name << '\t'
       << rec.address << '\t' << id;
  std::string resp = impl_->primary ? Execute(*impl_, line.str())
                                    : RemoteRequest(*impl_, line.str());
  if (resp.empty()) return false;
  if (!impl_->primary) {
    std::lock_guard<std::mutex> dispatch(impl_->dispatch_mutex_);
    std::lock_guard<std::mutex> data(impl_->data_mutex_);
    if (impl_->records_.count(id) != 0) ApplyLocked(*impl_, rec, false);
  }
  return true;
}

std::vector<EndpointRecord> Registry::Query(RegistryKind kind,
                                            const std::string& name,
                                            const std::string& backend) {
  if (!Start()) return {};
  if (!impl_->primary) {
    std::vector<EndpointRecord> remote;
    if (!QueryRemote(*impl_, kind, name, backend, &remote)) return {};
    return remote;
  }
  std::ostringstream line;
  line << "QLIST\t" << KindToString(kind) << '\t' << name << '\t' << backend;
  std::vector<EndpointRecord> records;
  if (ParseRecords(Execute(*impl_, line.str()), kind, name, &records)) {
    return records;
  }
  // 旧版主实例不认识 QLIST：退回原 LIST 文本协议并派生兼容字段。
  line.str(std::string());
  line.clear();
  line << "LIST\t" << KindToString(kind) << '\t' << name;
  if (!ParseLegacyList(Execute(*impl_, line.str()), kind, name, backend,
                       &records)) {
    return {};
  }
  return records;
}

bool Registry::Has(Kind kind, const std::string& name) {
  if (!Start()) return false;
  std::string line = "HAS\t" + std::string(KindToString(kind)) + "\t" + name;
  std::string resp =
      impl_->primary ? Execute(*impl_, line) : RemoteRequest(*impl_, line);
  return resp == "OK 1\n";
}

int Registry::Watch(Kind kind, const std::string& name, WatchCallback&& cb) {
  if (!Start()) return 0;
  int id = 0;
  std::vector<EndpointRecord> snapshot;
  // Follower 的本地缓存只包含启动后收到的广播；启动前已注册的记录必须
  // 主动向主实例取快照，否则晚启动 Reader 永远 dial 不到已有 Writer。
  // 查询在 dispatch_mutex_ 之外完成，避免阻塞广播派发；查询期间的变更由
  // 修订号裁决，保证初始快照不覆盖更新的增量。
  std::vector<EndpointRecord> remote;
  std::uint64_t query_revision = 0;
  bool have_snapshot = false;
  if (!impl_->primary) {
    {
      std::lock_guard<std::mutex> data(impl_->data_mutex_);
      query_revision = impl_->revision_;
    }
    have_snapshot =
        QueryRemote(*impl_, kind, name, std::string(), &remote);
  }
  {
    std::lock_guard<std::mutex> dispatch(impl_->dispatch_mutex_);
    {
      std::lock_guard<std::mutex> sub(impl_->sub_mutex_);
      id = impl_->next_watch_id_++;
      impl_->watches_.push_back(WatchEntry{id, kind, name, std::move(cb)});
    }
    // 查询失败时保留本地缓存：失败不代表端点不存在，等下一轮对账。
    if (have_snapshot) {
      MergeSnapshotLocked(*impl_, kind, name, remote, query_revision);
    }
    {
      std::lock_guard<std::mutex> data(impl_->data_mutex_);
      snapshot = SnapshotLocked(*impl_, kind, name);
    }
    if (id != 0 && !impl_->watches_.empty()) {
      WatchCallback initial;
      {
        std::lock_guard<std::mutex> sub(impl_->sub_mutex_);
        for (const auto& w : impl_->watches_) {
          if (w.id == id) initial = w.cb;
        }
      }
      if (initial) {
        try {
          initial(snapshot);
        } catch (...) {
          FY_ERROR("registry: watch callback threw");
        }
      }
    }
  }
  return id;
}

void Registry::Unwatch(int id) {
  std::lock_guard<std::mutex> dispatch(impl_->dispatch_mutex_);
  std::lock_guard<std::mutex> sub(impl_->sub_mutex_);
  auto& watches = impl_->watches_;
  watches.erase(std::remove_if(watches.begin(), watches.end(),
                               [id](const WatchEntry& w) { return w.id == id; }),
                watches.end());
}

}  // namespace discovery
}  // namespace firefly
