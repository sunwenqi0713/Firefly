// Service / Client 单元测试：覆盖服务就绪等待、同步/异步请求、超时与失败路径。
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>

#include "firefly/firefly.h"
#include "test_messages.pb.h"

using firefly::test::CalcRequest;
using firefly::test::CalcResponse;

namespace {

constexpr auto kWaitReady = std::chrono::seconds(3);

std::shared_ptr<CalcRequest> MakeReq(long long a, long long b) {
  auto req = std::make_shared<CalcRequest>();
  req->set_a(a);
  req->set_b(b);
  return req;
}

}  // namespace

int main() {
  firefly::Init("client_test");
  auto node = firefly::CreateNode("client_test_node");

  // ---------- 服务端 ----------
  auto service = node->CreateService<CalcRequest, CalcResponse>(
      "calc",
      [](const std::shared_ptr<const CalcRequest>& req)
          -> firefly::ServiceResult<CalcResponse> {
        auto resp = std::make_shared<CalcResponse>();
        resp->set_sum(req->a() + req->b());
        return {firefly::ErrorCode::kOk, resp};
      }).value;
  if (!service) return 10;

  // ---------- 客户端就绪等待 ----------
  auto client = node->CreateClient<CalcRequest, CalcResponse>("calc").value;
  if (!client) return 20;

  if (!client->WaitForService(kWaitReady)) return 21;  // 限时等待
  if (!client->WaitForService(std::chrono::seconds(-1))) return 22;  // 无限等待
  if (!client->ServiceIsReady()) return 23;
  auto quick = client->SendRequest(MakeReq(1, 1), std::chrono::milliseconds(100));
  if (!quick || quick.code != firefly::ErrorCode::kOk) {
    return 24;  // 已就绪，快请求不应失败
  }

  // ---------- 同步请求（shared_ptr / 值语义） ----------
  auto resp = client->SendRequest(MakeReq(10, 32), std::chrono::seconds(2));
  if (!resp || resp.code != firefly::ErrorCode::kOk || resp->sum() != 42) return 30;

  CalcRequest by_value;
  by_value.set_a(-5);
  by_value.set_b(47);
  auto resp2 = client->SendRequest(by_value, std::chrono::seconds(2));
  if (!resp2 || resp2.code != firefly::ErrorCode::kOk || resp2->sum() != 42) return 31;

  // ---------- 异步请求 ----------
  auto fut = client->AsyncSendRequest(MakeReq(7, 8));
  auto async_resp = fut.get();
  if (async_resp == nullptr || async_resp->sum() != 15) return 40;

  // ---------- 异步请求 + 回调 ----------
  std::promise<long long> done;
  auto fut_cb = client->AsyncSendRequest(
      MakeReq(100, 23),
      [&done](std::shared_future<std::shared_ptr<CalcResponse>>& f) {
        auto r = f.get();
        done.set_value(r ? r->sum() : -1);
      });
  (void)fut_cb;
  if (done.get_future().get() != 123) return 50;

  // ---------- 客户端指向不存在的服务 ----------
  auto ghost = node->CreateClient<CalcRequest, CalcResponse>("ghost_service").value;
  if (!ghost) return 60;
  if (ghost->ServiceIsReady()) return 61;
  if (ghost->WaitForService(std::chrono::milliseconds(200))) return 62;
  if (ghost->SendRequest(MakeReq(1, 1), std::chrono::milliseconds(300)).code !=
      firefly::ErrorCode::kServiceNotFound) {
    return 63;  // 无服务端时同步请求应返回 kServiceNotFound
  }

  // ---------- 服务 Shutdown 后不可用 ----------
  service->Shutdown();
  if (client->WaitForService(std::chrono::milliseconds(300))) return 70;
  if (!node->CreateClient<CalcRequest, CalcResponse>("calc")) {
    return 71;
  }

  firefly::Clear();
  return 0;
}
