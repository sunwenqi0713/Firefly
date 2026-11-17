// 使用示例：服务 / 客户端（Service / Client）
//
// 演示：
//   1. Service 处理请求并返回响应
//   2. 同步 SendRequest（shared_ptr 与值语义两种重载）
//   3. 异步 AsyncSendRequest（future 等待 + 回调通知）
//   4. WaitForService / ServiceIsReady 服务可用性等待
//
// 消息类型使用 tests/test_messages.proto 生成的 firefly::test::Calc*。

#include <chrono>
#include <cstdio>
#include <future>
#include <memory>

#include "firefly/firefly.h"
#include "test_messages.pb.h"

using firefly::test::CalcRequest;
using firefly::test::CalcResponse;

int main() {
  firefly::Init("rpc_calculator_example");
  auto node = firefly::CreateNode("calc_node").value;

  // ---- 服务：计算 a + b ----
  auto service = node->CreateService<CalcRequest, CalcResponse>(
      "calc",
      [](const std::shared_ptr<const CalcRequest>& req)
          -> firefly::ServiceResult<CalcResponse> {
        auto resp = std::make_shared<CalcResponse>();
        resp->set_sum(req->a() + req->b());
        return {firefly::ErrorCode::kOk, resp};
      }).value;
  if (!service) return 10;

  auto client = node->CreateClient<CalcRequest, CalcResponse>("calc").value;
  if (!client) return 11;

  // ---- 等待服务就绪 ----
  if (!client->WaitForService(std::chrono::seconds(2))) return 12;
  std::printf("ServiceIsReady = %d\n", client->ServiceIsReady() ? 1 : 0);

  // ---- 同步请求（shared_ptr 语义）----
  auto req = std::make_shared<CalcRequest>();
  req->set_a(10);
  req->set_b(32);
  auto resp = client->SendRequest(req, std::chrono::seconds(2));
  if (!resp || resp.code != firefly::ErrorCode::kOk) return 13;
  std::printf("sync 10 + 32 = %lld\n", static_cast<long long>(resp->sum()));

  // ---- 同步请求（值语义重载）----
  CalcRequest req2;
  req2.set_a(-5);
  req2.set_b(47);
  auto resp2 = client->SendRequest(req2, std::chrono::seconds(2));
  if (!resp2 || resp2.code != firefly::ErrorCode::kOk ||
      resp2->sum() != 42) return 14;
  std::printf("value (-5) + 47 = %lld\n",
              static_cast<long long>(resp2->sum()));

  // ---- 异步请求：future 等待 ----
  auto fut = client->AsyncSendRequest(req);
  auto async_resp = fut.future.get();
  if (async_resp == nullptr) return 15;
  std::printf("async 10 + 32 = %lld\n",
              static_cast<long long>(async_resp->sum()));

  // ---- 异步请求：回调通知 ----
  std::promise<bool> done;
  auto fut_cb = client->AsyncSendRequest(
      req, [&done](std::shared_future<std::shared_ptr<CalcResponse>>& f) {
        auto r = f.get();
        std::printf("callback 10 + 32 = %lld\n",
                    r ? static_cast<long long>(r->sum()) : -1LL);
        done.set_value(r != nullptr);
      });
  if (!done.get_future().get()) return 16;
  (void)fut_cb;

  firefly::Clear();
  return 0;
}
