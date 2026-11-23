// interop_helper.cpp：旧协议进程（raw NNG + 旧线格式）与新版公开 API 进程互测。
// 用法：firefly_interop_helper <mode>
//   legacy_pub / new_sub      : 旧发布者 → 新订阅者
//   new_pub / legacy_sub      : 新发布者 → 旧订阅者
//   legacy_rep / new_req      : 旧服务 → 新客户端（FFRP 响应帧）
//   new_rep / legacy_req      : 新服务 → 旧客户端（FFRP 响应帧）
//   new_reg_primary / legacy_reg_client
//                             : 旧版注册协议客户端 → 新版 Primary
//   legacy_reg_primary / new_reg_follower
//                             : 新版 Follower → 旧版 Primary（注册/查询/注销/Watch）
//   new_pub_first / new_sub_late
//                             : 同版本双进程，Writer 先注册、Reader 后启动
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <nng/nng.h>
#include <nng/protocol/pubsub0/pub.h>
#include <nng/protocol/pubsub0/sub.h>
#include <nng/protocol/reqrep0/rep.h>
#include <nng/protocol/reqrep0/req.h>

#include "firefly/detail/interfaces.h"
#include "firefly/detail/net.h"
#include "firefly/detail/rpc_frame.h"
#include "firefly/firefly.h"
#include "test_messages.pb.h"

namespace {

using namespace std::chrono_literals;
using firefly::detail::IRegistry;
using firefly::detail::EndpointRecord;
using firefly::detail::EndpointRecordId;
using firefly::detail::RegistryKind;
using firefly::test::CalcRequest;
using firefly::test::CalcResponse;
using firefly::test::Chatter;

constexpr const char* kChannel = "interop/chat";
constexpr const char* kService = "interop_calc";

std::shared_ptr<IRegistry> Registry() {
  return firefly::detail::DefaultRegistry();
}

EndpointRecordId RegisterLegacy(IRegistry* reg, RegistryKind kind,
                                const char* name, const std::string& address) {
  EndpointRecord rec;
  rec.kind = kind;
  rec.name = name;
  rec.address = address;
  return reg->Register(rec);
}

std::vector<std::string> ListAddresses(IRegistry* reg, RegistryKind kind,
                                       const std::string& name) {
  std::vector<std::string> out;
  for (const auto& rec : reg->Query(kind, name)) {
    if (!rec.address.empty()) out.push_back(rec.address);
  }
  return out;
}

std::string DiscoveryUrl(const char* name, const char* fallback) {
  const char* value = std::getenv(name);
  return (value != nullptr && *value != '\0') ? std::string(value)
                                              : std::string(fallback);
}

bool WaitRegistered(IRegistry* reg, RegistryKind kind, const std::string& name,
                    bool want_present, std::chrono::seconds limit) {
  const auto deadline = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < deadline) {
    if (reg->Has(kind, name) == want_present) return true;
    std::this_thread::sleep_for(20ms);
  }
  return false;
}

int RecvRaw(nng_socket sock, std::string* out, int timeout_ms) {
  nng_setopt_ms(sock, NNG_OPT_RECVTIMEO, timeout_ms);
  void* data = nullptr;
  size_t size = 0;
  int r = nng_recv(sock, &data, &size, NNG_FLAG_ALLOC);
  if (r != 0) return r;
  out->assign(static_cast<char*>(data), size);
  nng_free(data, size);
  return 0;
}

int SendRaw(nng_socket sock, const std::string& bytes) {
  return nng_send(sock, const_cast<char*>(bytes.data()), bytes.size(), 0);
}

std::string Serialize(const Chatter& msg) {
  std::string out;
  msg.SerializeToString(&out);
  return out;
}

std::string Serialize(const CalcRequest& msg) {
  std::string out;
  msg.SerializeToString(&out);
  return out;
}

std::string Serialize(const CalcResponse& msg) {
  std::string out;
  msg.SerializeToString(&out);
  return out;
}

std::string MakeChatter(int seq) {
  Chatter msg;
  msg.set_seq(seq);
  msg.set_content("interop-" + std::to_string(seq));
  return Serialize(msg);
}

int LegacyPub() {
  nng_socket sock = NNG_SOCKET_INITIALIZER;
  if (nng_pub0_open(&sock) != 0) return 1;
  std::string url;
  nng_listener listener = NNG_LISTENER_INITIALIZER;
  if (!firefly::detail::ListenOnEphemeralPort(sock, &listener, &url)) return 2;
  auto reg = Registry();
  const auto id = RegisterLegacy(reg.get(), RegistryKind::kWriter, kChannel, url);
  if (id == 0) return 3;
  if (!WaitRegistered(reg.get(), RegistryKind::kReader, kChannel, true, 10s)) {
    return 4;
  }
  // 旧发布者没有订阅确认协议：重复发送同一序列，直到新订阅者完成 dial。
  // 新订阅者按 seq==seen+1 接受，重复帧会被忽略，线协议不变。
  for (int round = 0; round < 5; ++round) {
    for (int seq = 1; seq <= 3; ++seq) {
      if (SendRaw(sock, MakeChatter(seq)) != 0) return 5;
      std::this_thread::sleep_for(50ms);
    }
  }
  std::printf("LEGACY_PUB_OK\n");
  std::this_thread::sleep_for(1500ms);
  reg->Unregister(id);
  nng_close(sock);
  return 0;
}

int NewSub() {
  auto node = firefly::CreateNode("interop_new_sub").value;
  if (!node) return 10;
  std::promise<void> done;
  auto future = done.get_future();
  std::atomic<int> seen{0};
  auto reader = node->CreateReader<Chatter>(
      kChannel, [&](const firefly::MessageContext&,
                    const std::shared_ptr<const Chatter>& msg) {
        if (msg->seq() == seen.load() + 1) {
          seen.fetch_add(1);
          if (seen.load() == 3) done.set_value();
        }
        return firefly::CallbackResult::kOk;
      }).value;
  if (!reader) return 11;
  std::printf("NEW_SUB_READY\n");
  std::fflush(stdout);
  if (future.wait_for(15s) != std::future_status::ready) return 12;
  if (seen.load() != 3) return 13;
  std::printf("NEW_SUB_OK seq=3\n");
  return 0;
}

int NewPub() {
  auto node = firefly::CreateNode("interop_new_pub").value;
  if (!node) return 20;
  auto writer = node->CreateWriter<Chatter>(kChannel).value;
  if (!writer) return 21;
  std::printf("NEW_PUB_READY\n");
  std::fflush(stdout);
  auto reg = Registry();
  if (!WaitRegistered(reg.get(), RegistryKind::kReader, kChannel, true, 10s)) {
    return 22;
  }
  // 旧订阅者要连续收 1..3；重复发送覆盖新发布者注册后 SUB dial 的时间窗。
  for (int round = 0; round < 5; ++round) {
    for (int seq = 1; seq <= 3; ++seq) {
      Chatter msg;
      msg.set_seq(seq);
      msg.set_content("interop-" + std::to_string(seq));
      const auto status = writer->Write(msg);
      // 旧订阅者收满 3 帧后先退出，后续发送的 kNoSubscriber 是正常收尾。
      if (status != firefly::WriteStatus::kAccepted &&
          status != firefly::WriteStatus::kNoSubscriber) {
        return 23;
      }
      std::this_thread::sleep_for(50ms);
    }
  }
  std::printf("NEW_PUB_OK\n");
  std::this_thread::sleep_for(1500ms);
  return 0;
}

int LegacySub() {
  nng_socket sock = NNG_SOCKET_INITIALIZER;
  if (nng_sub0_open(&sock) != 0) return 30;
  if (nng_setopt(sock, NNG_OPT_SUB_SUBSCRIBE, "", size_t(0)) != 0) return 31;
  auto reg = Registry();
  const auto id = RegisterLegacy(reg.get(), RegistryKind::kReader, kChannel, "");
  if (id == 0) return 32;
  if (!WaitRegistered(reg.get(), RegistryKind::kWriter, kChannel, true, 10s)) {
    return 33;
  }
  for (const auto& url : ListAddresses(reg.get(), RegistryKind::kWriter, kChannel)) {
    if (nng_dial(sock, url.c_str(), nullptr, 0) != 0) return 34;
  }
  for (int seq = 1; seq <= 3; ++seq) {
    std::string bytes;
    if (RecvRaw(sock, &bytes, 5000) != 0) return 35;
    Chatter msg;
    if (!msg.ParseFromString(bytes) || msg.seq() != seq) return 36;
  }
  std::printf("LEGACY_SUB_OK seq=3\n");
  reg->Unregister(id);
  nng_close(sock);
  return 0;
}

int LegacyRep() {
  nng_socket sock = NNG_SOCKET_INITIALIZER;
  if (nng_rep0_open(&sock) != 0) return 40;
  std::string url;
  nng_listener listener = NNG_LISTENER_INITIALIZER;
  if (!firefly::detail::ListenOnEphemeralPort(sock, &listener, &url)) return 41;
  auto reg = Registry();
  const auto id = RegisterLegacy(reg.get(), RegistryKind::kService, kService, url);
  if (id == 0) return 42;
  std::printf("LEGACY_REP_READY\n");
  std::fflush(stdout);
  for (int i = 0; i < 2; ++i) {
    std::string request;
    if (RecvRaw(sock, &request, 10000) != 0) return 43;
    CalcRequest req;
    if (!req.ParseFromString(request)) return 44;
    CalcResponse resp;
    resp.set_sum(req.a() + req.b());
    // 旧版响应帧：FFRP + uint32 status + protobuf payload，格式保持不变。
    std::string frame = firefly::detail::EncodeResponse(
        firefly::ErrorCode::kOk, Serialize(resp));
    if (SendRaw(sock, frame) != 0) return 45;
  }
  std::printf("LEGACY_REP_OK\n");
  std::this_thread::sleep_for(300ms);
  reg->Unregister(id);
  nng_close(sock);
  return 0;
}

int NewReq() {
  auto node = firefly::CreateNode("interop_new_req").value;
  if (!node) return 50;
  auto client = node->CreateClient<CalcRequest, CalcResponse>(kService).value;
  if (!client) return 51;
  if (!client->WaitForService(10s)) return 52;
  const std::pair<int, int> cases[] = {{10, 32}, {7, 5}};
  for (auto c : cases) {
    auto req = std::make_shared<CalcRequest>();
    req->set_a(c.first);
    req->set_b(c.second);
    auto resp = client->SendRequest(req, 5s);
    if (!resp || resp.code != firefly::ErrorCode::kOk ||
        resp->sum() != c.first + c.second) {
      return 53;
    }
  }
  std::printf("NEW_REQ_OK\n");
  return 0;
}

int NewRep() {
  auto node = firefly::CreateNode("interop_new_rep").value;
  if (!node) return 60;
  std::promise<void> done;
  auto future = done.get_future();
  std::atomic<int> calls{0};
  auto service = node->CreateService<CalcRequest, CalcResponse>(
      kService, [&](const std::shared_ptr<const CalcRequest>& req)
                    -> firefly::ServiceResult<CalcResponse> {
        auto resp = std::make_shared<CalcResponse>();
        resp->set_sum(req->a() + req->b());
        if (calls.fetch_add(1) + 1 == 2) done.set_value();
        return {firefly::ErrorCode::kOk, resp};
      }).value;
  if (!service) return 61;
  std::printf("NEW_REP_READY\n");
  std::fflush(stdout);
  if (future.wait_for(20s) != std::future_status::ready) return 62;
  std::printf("NEW_REP_OK\n");
  // 主注册中心过早退出会让 follower 的迟到注销/请求失败；留出旧请求进程收尾时间。
  std::this_thread::sleep_for(1000ms);
  return 0;
}

int LegacyReq() {
  auto reg = Registry();
  if (!WaitRegistered(reg.get(), RegistryKind::kService, kService, true, 10s)) {
    return 70;
  }
  auto urls = ListAddresses(reg.get(), RegistryKind::kService, kService);
  if (urls.empty()) return 71;
  nng_socket sock = NNG_SOCKET_INITIALIZER;
  if (nng_req0_open(&sock) != 0) return 72;
  if (nng_dial(sock, urls.front().c_str(), nullptr, 0) != 0) return 73;
  const std::pair<int, int> cases[] = {{10, 32}, {7, 5}};
  for (auto c : cases) {
    CalcRequest req;
    req.set_a(c.first);
    req.set_b(c.second);
    if (SendRaw(sock, Serialize(req)) != 0) return 74;
    std::string frame;
    if (RecvRaw(sock, &frame, 5000) != 0) return 75;
    firefly::ErrorCode code = firefly::ErrorCode::kSerialization;
    std::string payload;
    if (!firefly::detail::DecodeResponse(frame, &code, &payload) ||
        code != firefly::ErrorCode::kOk) {
      return 76;
    }
    CalcResponse resp;
    if (!resp.ParseFromString(payload) || resp.sum() != c.first + c.second) {
      return 77;
    }
  }
  std::printf("LEGACY_REQ_OK\n");
  nng_close(sock);
  return 0;
}

// ---------------------------------------------------------------------------
// 发现协议兼容：严格按旧版（4aecd32）的 4 字段请求与 "OK\n" 判断实现。
// ---------------------------------------------------------------------------
constexpr const char* kLegacyRegName = "legacy/reg";
constexpr const char* kLegacyRegAddress = "legacy://reg";
constexpr const char* kLegacySvcName = "legacy/reg/svc";
constexpr const char* kLegacySvcAddress = "legacy://service";

int LegacyRegClient() {
  nng_socket req = NNG_SOCKET_INITIALIZER;
  if (nng_req0_open(&req) != 0) return 80;
  const auto url = DiscoveryUrl("FIREFLY_DISCOVERY_REP_URL",
                                "tcp://127.0.0.1:37221");
  if (nng_dial(req, url.c_str(), nullptr, 0) != 0) {
    nng_close(req);
    return 81;
  }
  // 旧版调用方严格比较完整响应；新版返回 "OK <id>\n" 会被误判为失败。
  auto strict = [&](const std::string& line, const std::string& expected) {
    if (SendRaw(req, line) != 0) return false;
    std::string resp;
    return RecvRaw(req, &resp, 3000) == 0 && resp == expected;
  };
  const std::string reg = std::string("REG\twriter\t") + kLegacyRegName + "\t" +
                          kLegacyRegAddress;
  if (!strict(reg, "OK\n")) return 82;
  if (!strict(std::string("HAS\twriter\t") + kLegacyRegName, "OK 1\n")) return 83;
  if (!strict(std::string("LIST\twriter\t") + kLegacyRegName,
              std::string("OK 1\n") + kLegacyRegAddress + "\n")) return 84;
  // 给主实例一次轮询窗口观察 HAS=true（REQ/REP 连续调用可能快于 20ms 轮询）。
  std::this_thread::sleep_for(1000ms);
  const std::string unreg = std::string("UNREG\twriter\t") + kLegacyRegName +
                            "\t" + kLegacyRegAddress;
  if (!strict(unreg, "OK\n")) return 85;
  if (!strict(std::string("HAS\twriter\t") + kLegacyRegName, "OK 0\n")) return 86;
  nng_close(req);
  std::printf("LEGACY_REG_OK\n");
  return 0;
}

int NewRegPrimary() {
  auto reg = Registry();
  EndpointRecord self;
  self.kind = RegistryKind::kReader;
  self.name = "legacy/reg/observer";
  const auto id = reg->Register(self);
  if (id == 0) return 88;
  std::printf("NEW_REG_PRIMARY_READY\n");
  std::fflush(stdout);
  if (!WaitRegistered(reg.get(), RegistryKind::kWriter, kLegacyRegName, true,
                      15s)) {
    return 89;
  }
  auto records = reg->Query(RegistryKind::kWriter, kLegacyRegName);
  if (records.size() != 1 || records.front().address != kLegacyRegAddress) {
    return 90;
  }
  const auto deadline = std::chrono::steady_clock::now() + 15s;
  while (reg->Has(RegistryKind::kWriter, kLegacyRegName) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(20ms);
  }
  if (reg->Has(RegistryKind::kWriter, kLegacyRegName)) return 91;
  reg->Unregister(id);
  firefly::Clear();
  std::printf("NEW_REG_PRIMARY_OK\n");
  return 0;
}

// 旧版 Primary 的最小实现：只认 4 字段 REG/UNREG、LIST/HAS，响应恒为
// "OK\n"；广播为 4 段 "A|kind|name|url"。在启动屏障前预置一个 service，
// 供新版 Follower 的 Watch 初始快照验证。
int LegacyRegPrimary() {
  nng_socket rep = NNG_SOCKET_INITIALIZER;
  nng_socket pub = NNG_SOCKET_INITIALIZER;
  if (nng_rep0_open(&rep) != 0) return 92;
  if (nng_pub0_open(&pub) != 0) {
    nng_close(rep);
    return 93;
  }
  const auto rep_url = DiscoveryUrl("FIREFLY_DISCOVERY_REP_URL",
                                    "tcp://127.0.0.1:37221");
  const auto pub_url = DiscoveryUrl("FIREFLY_DISCOVERY_PUB_URL",
                                    "tcp://127.0.0.1:37222");
  nng_setopt_ms(rep, NNG_OPT_RECVTIMEO, 200);
  if (nng_listen(rep, rep_url.c_str(), nullptr, 0) != 0 ||
      nng_listen(pub, pub_url.c_str(), nullptr, 0) != 0) {
    nng_close(rep);
    nng_close(pub);
    return 94;
  }
  std::map<std::string, std::set<std::string>> urls;
  // 屏障之前就存在的记录：新版 Follower 的 Watch 初始快照必须能拿到它。
  urls[std::string("service\t") + kLegacySvcName].insert(kLegacySvcAddress);
  std::printf("LEGACY_REG_PRIMARY_READY\n");
  std::fflush(stdout);
  const auto deadline = std::chrono::steady_clock::now() + 15s;
  while (std::chrono::steady_clock::now() < deadline) {
    std::string req;
    if (RecvRaw(rep, &req, 200) != 0) continue;
    std::istringstream in(req);
    std::string cmd, kind, name, url;
    std::getline(in, cmd, '\t');
    std::getline(in, kind, '\t');
    std::getline(in, name, '\t');
    std::getline(in, url, '\t');
    const std::string key = kind + "\t" + name;
    std::string resp;
    if (cmd == "REG" || cmd == "UNREG") {
      if (cmd == "REG") {
        urls[key].insert(url);
      } else {
        auto it = urls.find(key);
        if (it != urls.end()) {
          it->second.erase(url);
          if (it->second.empty()) urls.erase(it);
        }
      }
      // 旧版广播没有 id/backend 字段。
      SendRaw(pub, std::string(cmd == "REG" ? "A|" : "R|") + kind + "|" + name +
                       "|" + url);
      resp = "OK\n";
    } else if (cmd == "LIST" || cmd == "HAS") {
      auto it = urls.find(key);
      bool has = it != urls.end() && !it->second.empty();
      if (cmd == "HAS") {
        resp = has ? "OK 1\n" : "OK 0\n";
      } else {
        std::ostringstream out;
        out << "OK " << (has ? it->second.size() : 0) << "\n";
        if (has) {
          for (const auto& u : it->second) out << u << "\n";
        }
        resp = out.str();
      }
    } else {
      resp = "ERR unknown cmd\n";
    }
    SendRaw(rep, resp);
  }
  nng_close(rep);
  nng_close(pub);
  std::printf("LEGACY_REG_PRIMARY_OK\n");
  return 0;
}

int NewRegFollower() {
  auto reg = Registry();
  EndpointRecord rec;
  rec.kind = RegistryKind::kWriter;
  rec.name = kLegacyRegName;
  rec.address = "new://writer";
  const auto id = reg->Register(rec);  // 旧主实例占端口，本进程成为 Follower
  if (id == 0) return 96;
  auto found = reg->Query(RegistryKind::kWriter, kLegacyRegName);
  if (std::find_if(found.begin(), found.end(), [](const EndpointRecord& r) {
        return r.address == "new://writer";
      }) == found.end()) {
    return 97;
  }
  // Watch 的初始快照必须包含旧主实例在本进程启动前登记的服务：这正是
  // 旧版 LIST 回退 + Follower 主动对账要覆盖的场景。
  std::promise<std::vector<std::string>> snapshot;
  auto future = snapshot.get_future();
  std::atomic<bool> once{false};
  const int watch = reg->Watch(
      RegistryKind::kService, kLegacySvcName,
      [&](const std::vector<EndpointRecord>& records) {
        if (once.exchange(true)) return;
        std::vector<std::string> addresses;
        for (const auto& r : records) addresses.push_back(r.address);
        snapshot.set_value(std::move(addresses));
      });
  if (watch == 0) return 98;
  if (future.wait_for(10s) != std::future_status::ready) return 99;
  auto addresses = future.get();
  if (std::find(addresses.begin(), addresses.end(), kLegacySvcAddress) ==
      addresses.end()) {
    return 100;
  }
  reg->Unwatch(watch);
  if (!reg->Unregister(id)) return 101;
  if (reg->Has(RegistryKind::kWriter, kLegacyRegName)) return 102;
  firefly::Clear();
  std::printf("NEW_REG_FOLLOWER_OK\n");
  return 0;
}

// 同版本双进程：Writer 先注册并持续发布，Reader 随后启动。旧实现中
// Follower 的 Watch 只读本地广播缓存，永远 dial 不到启动前的 Writer。
int NewPubFirst() {
  auto node = firefly::CreateNode("interop_new_pub_first").value;
  if (!node) return 110;
  auto writer = node->CreateWriter<Chatter>("late/reader/ch").value;
  if (!writer) return 111;
  std::printf("NEW_PUB_FIRST_READY\n");
  std::fflush(stdout);
  for (int round = 0; round < 60; ++round) {
    Chatter msg;
    msg.set_seq(round % 3 + 1);
    msg.set_content("late-reader");
    (void)writer->Write(msg);
    std::this_thread::sleep_for(100ms);
  }
  std::printf("NEW_PUB_FIRST_OK\n");
  return 0;
}

int NewSubLate() {
  auto node = firefly::CreateNode("interop_new_sub_late").value;
  if (!node) return 112;
  std::promise<void> done;
  auto future = done.get_future();
  std::atomic<int> seen{0};
  auto reader = node->CreateReader<Chatter>(
      "late/reader/ch",
      [&](const firefly::MessageContext&,
          const std::shared_ptr<const Chatter>& msg) {
        if (msg->seq() == seen.load() + 1) {
          seen.fetch_add(1);
          if (seen.load() == 3) done.set_value();
        }
        return firefly::CallbackResult::kOk;
      }).value;
  if (!reader) return 113;
  if (future.wait_for(15s) != std::future_status::ready) return 114;
  std::printf("NEW_SUB_LATE_OK seq=3\n");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return 90;
  const std::string mode = argv[1];
  // 发现协议模式自行管理 Init/Clear（raw NNG 端点需要精确控制主/从角色）。
  if (mode == "legacy_reg_client") return LegacyRegClient();
  if (mode == "legacy_reg_primary") return LegacyRegPrimary();
  if (!firefly::Init(("interop_" + mode).c_str())) return 91;
  int rc = 92;
  if (mode == "legacy_pub") rc = LegacyPub();
  else if (mode == "new_sub") rc = NewSub();
  else if (mode == "new_pub") rc = NewPub();
  else if (mode == "legacy_sub") rc = LegacySub();
  else if (mode == "legacy_rep") rc = LegacyRep();
  else if (mode == "new_req") rc = NewReq();
  else if (mode == "new_rep") rc = NewRep();
  else if (mode == "legacy_req") rc = LegacyReq();
  else if (mode == "new_reg_primary") rc = NewRegPrimary();
  else if (mode == "new_reg_follower") rc = NewRegFollower();
  else if (mode == "new_pub_first") rc = NewPubFirst();
  else if (mode == "new_sub_late") rc = NewSubLate();
  firefly::Clear();
  return rc;
}
