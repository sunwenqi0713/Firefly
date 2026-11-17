// request_cancel_test.cpp：RequestHandle 取消、超时兑现、Shutdown 等待在途（阶段二任务2）。
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "firefly/firefly.h"
#include "test_messages.pb.h"

using firefly::ErrorCode;
using firefly::test::CalcRequest;
using firefly::test::CalcResponse;

static int Check(bool cond, int line) { return cond ? 0 : line; }

static std::shared_ptr<const CalcRequest> MakeReq(int a, int b) {
  auto r = std::make_shared<CalcRequest>();
  r->set_a(a); r->set_b(b);
  return r;
}

int main() {
  firefly::Init("request_cancel_test");
  auto node = firefly::CreateNode("cancel_node").value;

  // ---------- 1. Cancel 后 future 以 kCancelled 兑现 ----------
  // 独立慢服务端（300ms），避免不同测试块间的 REQ socket 状态污染。
  {
    auto svc = node->CreateService<CalcRequest, CalcResponse>(
        "cancel_slow_svc",
        [](const std::shared_ptr<const CalcRequest>& req)
            -> firefly::ServiceResult<CalcResponse> {
          std::this_thread::sleep_for(std::chrono::milliseconds(300));
          auto r = std::make_shared<CalcResponse>();
          r->set_sum(req->a() + req->b());
          return {ErrorCode::kOk, r};
        }).value;
    auto client = node->CreateClient<CalcRequest, CalcResponse>("cancel_slow_svc").value;
    if (int e = Check(client->WaitForService(std::chrono::seconds(2)), 20)) return e;
    auto h = client->AsyncSendRequest(MakeReq(1, 2), std::chrono::milliseconds(2000));
    if (int e = Check(h.id != 0, 21)) return e;
    if (int e = Check(h.Cancel(), 22)) return e;
    bool threw_cancelled = false;
    try {
      (void)h.future.get();
    } catch (const firefly::FireflyException& ex) {
      threw_cancelled = (ex.code() == ErrorCode::kCancelled);
    }
    if (int e = Check(threw_cancelled, 23)) return e;
    if (int e = Check(h.Cancel() == false, 24)) return e;  // 重复 Cancel
    // 迟到响应：等服务端窗口过去，确认进程不崩溃
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    client->Shutdown();
    node->DeleteService("cancel_slow_svc");
    node->DeleteClient("cancel_slow_svc");
  }

  // ---------- 2. 超时以 kTimeout 兑现（不再永不兑现） ----------
  {
    auto svc = node->CreateService<CalcRequest, CalcResponse>(
        "timeout_slow_svc",
        [](const std::shared_ptr<const CalcRequest>& req)
            -> firefly::ServiceResult<CalcResponse> {
          std::this_thread::sleep_for(std::chrono::milliseconds(300));
          auto r = std::make_shared<CalcResponse>();
          r->set_sum(req->a() + req->b());
          return {ErrorCode::kOk, r};
        }).value;
    auto client = node->CreateClient<CalcRequest, CalcResponse>("timeout_slow_svc").value;
    if (int e = Check(client->WaitForService(std::chrono::seconds(2)), 30)) return e;
    auto h = client->AsyncSendRequest(MakeReq(1, 1), std::chrono::milliseconds(50));
    bool threw_timeout = false;
    try {
      (void)h.future.get();
    } catch (const firefly::FireflyException& ex) {
      threw_timeout = (ex.code() == ErrorCode::kTimeout);
    }
    if (int e = Check(threw_timeout, 31)) return e;
    client->Shutdown();
    node->DeleteService("timeout_slow_svc");
    node->DeleteClient("timeout_slow_svc");
  }

  // ---------- 3. Client::Shutdown 阻塞至在途请求兑现/取消 ----------
  {
    auto svc = node->CreateService<CalcRequest, CalcResponse>(
        "shutdown_slow_svc",
        [](const std::shared_ptr<const CalcRequest>& req)
            -> firefly::ServiceResult<CalcResponse> {
          std::this_thread::sleep_for(std::chrono::milliseconds(500));
          auto r = std::make_shared<CalcResponse>();
          r->set_sum(req->a() + req->b());
          return {ErrorCode::kOk, r};
        }).value;
    auto client = node->CreateClient<CalcRequest, CalcResponse>("shutdown_slow_svc").value;
    if (int e = Check(client->WaitForService(std::chrono::seconds(2)), 40)) return e;
    auto h = client->AsyncSendRequest(MakeReq(5, 6), std::chrono::milliseconds(3000));
    // 确保请求已到达服务端并开始处理（sleep 500ms）
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto t0 = std::chrono::steady_clock::now();
    client->Shutdown();  // 应阻塞：取消在途并等待工作线程从同步请求返回
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    if (int e = Check(elapsed >= std::chrono::milliseconds(200), 41)) return e;
    // 在途请求应以 kCancelled 兑现（Shutdown 取消语义）
    bool threw = false;
    try {
      (void)h.future.get();
    } catch (const firefly::FireflyException& ex) {
      threw = (ex.code() == ErrorCode::kCancelled);
    }
    if (int e = Check(threw, 42)) return e;
    // Shutdown 后 SendRequest 返回 kNotReady
    auto after = client->SendRequest(MakeReq(1, 1), std::chrono::milliseconds(200));
    if (int e = Check(!after && after.code == ErrorCode::kNotReady, 43)) return e;
    node->DeleteService("shutdown_slow_svc");
    node->DeleteClient("shutdown_slow_svc");
  }

  firefly::Clear();
  return 0;
}
