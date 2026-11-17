// rpc_error_test.cpp：同一 Client 上区分 timeout / service-not-found /
// 业务失败 / 反序列化失败四个 ErrorCode。
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>

#include "firefly/firefly.h"
#include "firefly/detail/rpc_frame.h"
#include "test_messages.pb.h"

using firefly::ErrorCode;
using firefly::ServiceResult;
using firefly::test::CalcRequest;
using firefly::test::CalcResponse;

static std::shared_ptr<CalcRequest> Req(long long a, long long b) {
  auto req = std::make_shared<CalcRequest>();
  req->set_a(a);
  req->set_b(b);
  return req;
}

int main() {
  firefly::Init("rpc_error_test");
  auto node = firefly::CreateNode("rpc_error_node").value;
  if (!node) return 1;

  // 服务端：a==1 模拟慢处理（触发客户端 timeout）；a==2 返回业务失败
  auto service = node->CreateService<CalcRequest, CalcResponse>(
      "err_calc",
      [](const std::shared_ptr<const CalcRequest>& req) -> ServiceResult<CalcResponse> {
        if (req->a() == 1) {
          std::this_thread::sleep_for(std::chrono::milliseconds(400));
        }
        if (req->a() == 2) {
          return {ErrorCode::kServiceFailed, nullptr};
        }
        auto resp = std::make_shared<CalcResponse>();
        resp->set_sum(req->a() + req->b());
        return {ErrorCode::kOk, resp};
      }).value;
  if (!service) return 10;

  auto client = node->CreateClient<CalcRequest, CalcResponse>("err_calc").value;
  if (!client) return 11;
  if (!client->WaitForService(std::chrono::seconds(2))) return 12;

  // 1) kTimeout
  auto r_timeout = client->SendRequest(Req(1, 1), std::chrono::milliseconds(50));
  if (r_timeout.code != ErrorCode::kTimeout) return 20;

  // 2) kServiceFailed（响应帧 status 字段带回）
  auto r_failed = client->SendRequest(Req(2, 2), std::chrono::seconds(2));
  if (r_failed.code != ErrorCode::kServiceFailed) return 21;

  // 3) kOk（基线，确认同一 Client 仍可用）
  auto r_ok = client->SendRequest(Req(10, 32), std::chrono::seconds(2));
  if (r_ok.code != ErrorCode::kOk || r_ok->sum() != 42) return 22;

  // 4) kServiceNotFound：指向不存在服务的 Client
  auto ghost = node->CreateClient<CalcRequest, CalcResponse>("ghost_svc").value;
  if (!ghost) return 30;
  auto r_missing = ghost->SendRequest(Req(1, 1), std::chrono::milliseconds(200));
  if (r_missing.code != ErrorCode::kServiceNotFound) return 31;

  // 5) kSerialization：malformed 响应帧（magic 不符）→ 客户端映射为 kSerialization。
  // 直接验证帧解码层；client_impl.h 中 DecodeResponse 失败即返回该码。
  ErrorCode decoded = ErrorCode::kOk;
  std::string payload;
  if (firefly::detail::DecodeResponse("BAD!MAGIC!!!", &decoded, &payload)) return 40;
  if (firefly::detail::DecodeResponse("FFRP", &decoded, &payload)) return 41;

  // 服务端报 kSerialization 也能通过帧传回客户端
  auto svc2 = node->CreateService<CalcRequest, CalcResponse>(
      "ser_err_svc",
      [](const std::shared_ptr<const CalcRequest>&)
          -> ServiceResult<CalcResponse> {
        return {ErrorCode::kSerialization, nullptr};
      }).value;
  if (!svc2) return 50;
  auto cli2 = node->CreateClient<CalcRequest, CalcResponse>("ser_err_svc").value;
  if (!cli2) return 51;
  if (!cli2->WaitForService(std::chrono::seconds(2))) return 52;
  auto r_ser = cli2->SendRequest(Req(1, 1), std::chrono::seconds(2));
  if (r_ser.code != ErrorCode::kSerialization) return 53;

  // 异步失败以 FireflyException 兑现
  auto fut = ghost->AsyncSendRequest(Req(1, 1));
  bool threw = false;
  try {
    (void)fut.future.get();
  } catch (const firefly::FireflyException& e) {
    threw = e.code() == ErrorCode::kServiceNotFound && std::string(e.what()).find("ghost_svc") != std::string::npos;
  }
  if (!threw) return 60;

  firefly::Clear();
  return 0;
}
