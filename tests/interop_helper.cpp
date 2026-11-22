// interop_helper.cpp：旧协议进程（raw NNG + 旧线格式）与新版公开 API 进程互测。
// 用法：firefly_interop_helper <mode>
//   legacy_pub / new_sub      : 旧发布者 → 新订阅者
//   new_pub / legacy_sub      : 新发布者 → 旧订阅者
//   legacy_rep / new_req      : 旧服务 → 新客户端（FFRP 响应帧）
//   new_rep / legacy_req      : 新服务 → 旧客户端（FFRP 响应帧）
#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <memory>
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

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return 90;
  const std::string mode = argv[1];
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
  firefly::Clear();
  return rc;
}
