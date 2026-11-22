// backend_contract_test.cpp：第 4 批后端无关契约。
// 同一个测试源用 argv[1]=memory|nng 各跑一遍，覆盖取消、绝对截止时间、
// Close() 跨线程唤醒与幂等、响应对应、回调顺序、独立注册。
// 并发时序由 promise/future/barrier 控制，不依赖 sleep。
#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "firefly/detail/backend_access.h"
#include "firefly/detail/interfaces.h"
#include "firefly/firefly.h"
#include "test_messages.pb.h"

using namespace std::chrono_literals;
using firefly::CallbackResult;
using firefly::ErrorCode;
using firefly::detail::EndpointRecord;
using firefly::detail::IEndpointFactory;
using firefly::detail::InjectionContext;
using firefly::detail::IRegistry;
using firefly::detail::RegistryKind;
using firefly::detail::TransferStatus;
using firefly::test::CalcRequest;
using firefly::test::CalcResponse;
using firefly::test::Chatter;

namespace {

int Check(bool cond, int line) { return cond ? 0 : line; }

template <class Predicate>
bool WaitUntil(Predicate pred, std::chrono::milliseconds limit) {
  const auto deadline = std::chrono::steady_clock::now() + limit;
  while (!pred()) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::yield();
  }
  return true;
}

using CalcFuture =
    firefly::Client<CalcRequest, CalcResponse>::SharedFuture;

ErrorCode SettledCode(CalcFuture future) {
  if (future.wait_for(3s) != std::future_status::ready) return ErrorCode::kNotReady;
  try {
    (void)future.get();
    return ErrorCode::kOk;
  } catch (const firefly::FireflyException& ex) {
    return ex.code();
  }
}

std::shared_ptr<const CalcRequest> MakeReq(int a, int b) {
  auto req = std::make_shared<CalcRequest>();
  req->set_a(a);
  req->set_b(b);
  return req;
}

Chatter MakeChatter(int seq) {
  Chatter msg;
  msg.set_seq(seq);
  msg.set_content("backend-contract");
  return msg;
}

std::shared_ptr<InjectionContext> MakeContext(const std::string& backend) {
  auto ctx = std::make_shared<InjectionContext>();
  if (backend == "memory") {
    ctx->endpoints = firefly::detail::MakeMemoryEndpointFactory();
    ctx->registry = firefly::detail::MakeMemoryRegistry();
    return ctx;
  }
#if FIREFLY_WITH_NNG
  if (backend == "nng") {
    ctx->endpoints = firefly::detail::MakeNngEndpointFactory();
    ctx->registry = firefly::detail::MakeNngRegistry();
    return ctx;
  }
#endif
  return nullptr;
}

// ---------- Close 跨线程唤醒 / 幂等 / 绝对截止时间 ----------
int TestCloseAndDeadline(const std::shared_ptr<InjectionContext>& ctx) {
  auto endpoints = ctx->endpoints;

  auto subscriber = endpoints->CreateSubscriber();
  if (!subscriber) return 100;
  std::promise<void> sub_entered;
  auto sub_entered_future = sub_entered.get_future();
  std::promise<TransferStatus> sub_done;
  auto sub_done_future = sub_done.get_future();
  std::thread sub_thread([&] {
    sub_entered.set_value();
    std::string bytes;
    sub_done.set_value(
        subscriber->Receive(&bytes, std::chrono::steady_clock::now() + 10s));
  });
  if (sub_entered_future.wait_for(2s) != std::future_status::ready) return 101;
  subscriber->Close();
  subscriber->Close();  // 幂等
  if (sub_done_future.wait_for(2s) != std::future_status::ready) return 102;
  if (sub_done_future.get() != TransferStatus::kClosed) return 103;
  sub_thread.join();
  std::string bytes;
  if (subscriber->Receive(&bytes, std::chrono::steady_clock::now() + 1s) !=
      TransferStatus::kClosed) {
    return 104;
  }

  auto subscriber2 = endpoints->CreateSubscriber();
  if (!subscriber2) return 105;
  const auto started = std::chrono::steady_clock::now();
  if (subscriber2->Receive(&bytes, started + 50ms) != TransferStatus::kTimeout) {
    return 106;
  }
  if (std::chrono::steady_clock::now() - started > 2s) return 107;

  auto responder = endpoints->CreateResponder();
  if (!responder) return 108;
  std::string endpoint;
  if (responder->Bind(&endpoint) != TransferStatus::kOk) return 109;
  std::promise<void> rep_entered;
  auto rep_entered_future = rep_entered.get_future();
  std::promise<TransferStatus> rep_done;
  auto rep_done_future = rep_done.get_future();
  std::thread rep_thread([&] {
    rep_entered.set_value();
    std::string request;
    std::unique_ptr<firefly::detail::ReplyToken> token;
    rep_done.set_value(responder->Receive(
        &request, &token, std::chrono::steady_clock::now() + 10s));
  });
  if (rep_entered_future.wait_for(2s) != std::future_status::ready) return 110;
  responder->Close();
  responder->Close();
  if (rep_done_future.wait_for(2s) != std::future_status::ready) return 111;
  if (rep_done_future.get() != TransferStatus::kClosed) return 112;
  rep_thread.join();
  return 0;
}

// ---------- 排队中取消：服务端 0 次执行 ----------
int TestCancelledRequest(const std::string& backend,
                         const std::shared_ptr<InjectionContext>& ctx) {
  const std::string service = "bc_" + backend + "_cancel_svc";
  auto node = firefly::CreateNode("bc_" + backend + "_cancel_node", "", ctx).value;
  if (!node) return 120;
  std::promise<void> entered;
  std::promise<void> release;
  auto release_future = release.get_future().share();
  std::atomic<int> executed{0};
  auto service_ptr = node->CreateService<CalcRequest, CalcResponse>(
      service, [&](const std::shared_ptr<const CalcRequest>& req)
                   -> firefly::ServiceResult<CalcResponse> {
        const int n = executed.fetch_add(1) + 1;
        if (n == 1) {
          entered.set_value();
          release_future.wait();
        }
        auto resp = std::make_shared<CalcResponse>();
        resp->set_sum(req->a() + req->b());
        return {ErrorCode::kOk, resp};
      }).value;
  if (!service_ptr) return 121;
  auto client = node->CreateClient<CalcRequest, CalcResponse>(service).value;
  if (!client->WaitForService(3s)) return 122;

  auto first = client->AsyncSendRequest(MakeReq(1, 1), 5s);
  if (entered.get_future().wait_for(2s) != std::future_status::ready) return 123;
  auto cancelled = client->AsyncSendRequest(MakeReq(2, 2), 5s);
  if (!cancelled.Cancel()) return 124;
  if (SettledCode(cancelled.future) != ErrorCode::kCancelled) return 125;
  release.set_value();
  if (SettledCode(first.future) != ErrorCode::kOk) return 126;
  if (executed.load() != 1) return 127;
  client->Shutdown();
  node->DeleteService(service);
  return 0;
}

// ---------- 绝对截止时间：排队请求出队时已到期 ----------
int TestAbsoluteDeadline(const std::string& backend,
                         const std::shared_ptr<InjectionContext>& ctx) {
  const std::string service = "bc_" + backend + "_deadline_svc";
  auto node = firefly::CreateNode("bc_" + backend + "_deadline_node", "", ctx).value;
  if (!node) return 140;
  std::promise<void> entered;
  std::promise<void> release;
  auto release_future = release.get_future().share();
  std::atomic<int> executed{0};
  auto service_ptr = node->CreateService<CalcRequest, CalcResponse>(
      service, [&](const std::shared_ptr<const CalcRequest>& req)
                   -> firefly::ServiceResult<CalcResponse> {
        const int n = executed.fetch_add(1) + 1;
        if (n == 1) {
          entered.set_value();
          release_future.wait();
        }
        auto resp = std::make_shared<CalcResponse>();
        resp->set_sum(req->a() + req->b());
        return {ErrorCode::kOk, resp};
      }).value;
  if (!service_ptr) return 141;
  auto client = node->CreateClient<CalcRequest, CalcResponse>(service).value;
  if (!client->WaitForService(3s)) return 142;

  auto first = client->AsyncSendRequest(MakeReq(3, 4), 5s);
  if (entered.get_future().wait_for(2s) != std::future_status::ready) return 143;
  auto expired = client->AsyncSendRequest(MakeReq(5, 6), 50ms);
  if (expired.future.wait_for(100ms) != std::future_status::timeout) return 144;
  release.set_value();
  if (SettledCode(expired.future) != ErrorCode::kTimeout) return 145;
  if (SettledCode(first.future) != ErrorCode::kOk) return 146;
  if (executed.load() != 1) return 147;
  client->Shutdown();
  node->DeleteService(service);
  return 0;
}

// ---------- 响应对应：每次请求拿回自己的响应 ----------
int TestResponseMatching(const std::string& backend,
                         const std::shared_ptr<InjectionContext>& ctx) {
  const std::string service = "bc_" + backend + "_echo_svc";
  auto node = firefly::CreateNode("bc_" + backend + "_echo_node", "", ctx).value;
  if (!node) return 160;
  auto service_ptr = node->CreateService<CalcRequest, CalcResponse>(
      service, [](const std::shared_ptr<const CalcRequest>& req)
                   -> firefly::ServiceResult<CalcResponse> {
        auto resp = std::make_shared<CalcResponse>();
        resp->set_sum(req->a() + req->b());
        return {ErrorCode::kOk, resp};
      }).value;
  if (!service_ptr) return 161;
  auto client = node->CreateClient<CalcRequest, CalcResponse>(service).value;
  if (!client->WaitForService(3s)) return 162;
  const std::pair<int, int> cases[] = {{10, 32}, {7, 5}, {-4, 9}};
  for (const auto& c : cases) {
    auto resp = client->SendRequest(MakeReq(c.first, c.second), 2s);
    if (!resp || resp.code != ErrorCode::kOk) return 163;
    if (resp->sum() != c.first + c.second) return 164;
  }
  client->Shutdown();
  node->DeleteService(service);
  return 0;
}

// ---------- 回调顺序：同一 Writer 的帧按提交顺序到达 ----------
int TestCallbackOrder(const std::string& backend,
                      const std::shared_ptr<InjectionContext>& ctx) {
  const std::string channel = "bc/" + backend + "/ordered";
  auto node = firefly::CreateNode("bc_" + backend + "_order_node", "", ctx).value;
  if (!node) return 180;
  auto writer = node->CreateWriter<Chatter>(channel).value;
  if (!writer) return 181;

  std::atomic<int> expected{0};
  std::atomic<bool> completed{false};
  std::promise<void> done;
  auto done_future = done.get_future().share();
  auto reader = node->CreateReader<Chatter>(
      channel, [&](const firefly::MessageContext&,
                   const std::shared_ptr<const Chatter>& msg) {
        if (msg->seq() == expected.load() + 1) {
          const int reached = expected.fetch_add(1) + 1;
          if (reached == 5 && !completed.exchange(true)) done.set_value();
        }
        return CallbackResult::kOk;
      }).value;
  if (!reader) return 182;
  if (!WaitUntil([&] { return writer->HasReader(); }, 3s)) return 183;

  for (int round = 0;
       round < 500 && done_future.wait_for(0ms) != std::future_status::ready;
       ++round) {
    for (int seq = 1; seq <= 5; ++seq) writer->Write(MakeChatter(seq));
    std::this_thread::yield();
  }
  if (done_future.wait_for(2s) != std::future_status::ready) return 184;
  if (expected.load() != 5) return 185;
  writer->Shutdown();
  return 0;
}

// ---------- 独立注册：同键多条记录按 id 独立注销 ----------
int TestIndependentRegistration(const std::string& backend,
                                const std::shared_ptr<InjectionContext>& ctx) {
  const std::string channel = "bc/" + backend + "/independent";
  auto node_a = firefly::CreateNode("bc_" + backend + "_reg_a", "", ctx).value;
  auto node_b = firefly::CreateNode("bc_" + backend + "_reg_b", "", ctx).value;
  if (!node_a || !node_b) return 200;
  auto reader_a = node_a->CreateReader<Chatter>(channel).value;
  auto reader_b = node_b->CreateReader<Chatter>(channel).value;
  if (!reader_a || !reader_b) return 201;
  auto reg = ctx->registry;
  if (!WaitUntil(
          [&] { return reg->Query(RegistryKind::kReader, channel).size() == 2; },
          3s)) {
    return 202;
  }
  if (!node_a->DeleteReader(channel)) return 203;
  if (node_a->GetReader<Chatter>(channel) != nullptr) return 204;
  if (node_b->GetReader<Chatter>(channel) == nullptr) return 205;
  if (reg->Query(RegistryKind::kReader, channel).size() != 1) return 206;
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string backend = argc > 1 ? argv[1] : "memory";
  auto ctx = MakeContext(backend);
  if (!ctx) {
    std::printf("backend_contract: unknown backend %s\n", backend.c_str());
    return 1;
  }
  if (!firefly::Init(("backend_contract_" + backend).c_str())) return 2;

  struct Case {
    const char* name;
    int (*fn)(const std::string&, const std::shared_ptr<InjectionContext>&);
  };
  struct RoleCase {
    const char* name;
    int (*fn)(const std::shared_ptr<InjectionContext>&);
  };
  const RoleCase role_cases[] = {{"close_and_deadline", TestCloseAndDeadline}};
  const Case cases[] = {
      {"cancelled_request", TestCancelledRequest},
      {"absolute_deadline", TestAbsoluteDeadline},
      {"response_matching", TestResponseMatching},
      {"callback_order", TestCallbackOrder},
      {"independent_registration", TestIndependentRegistration},
  };
  for (const auto& c : role_cases) {
    if (int e = c.fn(ctx)) {
      std::printf("backend_contract[%s]: %s FAILED at line %d\n",
                  backend.c_str(), c.name, e);
      return e;
    }
  }
  for (const auto& c : cases) {
    if (int e = c.fn(backend, ctx)) {
      std::printf("backend_contract[%s]: %s FAILED at line %d\n",
                  backend.c_str(), c.name, e);
      return e;
    }
  }
  firefly::Clear();
  std::printf("backend_contract[%s]: all %zu cases passed\n", backend.c_str(),
              sizeof(role_cases) / sizeof(role_cases[0]) +
                  sizeof(cases) / sizeof(cases[0]));
  return 0;
}
