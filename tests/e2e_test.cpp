// 绔埌绔祴璇?鍚屼竴杩涚▼鍐?Writer/Reader 鍏ㄩ摼璺?+ Service/Client銆?// 鍏变韩鍚屼竴涓敞鍐屼富瀹炰緥锛岄獙璇?PUB/SUB 涓?REQ/REP 鏀跺彂銆?#include <chrono>
#include <thread>

#include "firefly/firefly.h"
#include "test_messages.pb.h"

int main() {
  firefly::Init("firefly_e2e_test");
  auto node = firefly::CreateNode("test_node").value;

  // ---------- 鍙戝竷 / 璁㈤槄 ----------
  auto writer = node->CreateWriter<firefly::test::Chatter>("channel/chatter").value;
  if (!writer) return 10;

  auto reader = node->CreateReader<firefly::test::Chatter>(
      "channel/chatter",
      [](const firefly::MessageContext&,
         const std::shared_ptr<const firefly::test::Chatter>&) {
        return firefly::CallbackResult::kOk;
      }).value;
  if (!reader) return 11;

  std::this_thread::sleep_for(std::chrono::milliseconds(300));  // 绛夎闃呭缓绔?
  firefly::test::Chatter msg;
  msg.set_seq(42);
  msg.set_content("hello firofly");
  if (!writer->Write(msg)) return 12;

  std::this_thread::sleep_for(std::chrono::milliseconds(2000));
  if (!reader->HasReceived()) return 13;

  // ---------- 鏈嶅姟 / 瀹㈡埛绔?----------
  auto service = node->CreateService<firefly::test::CalcRequest, firefly::test::CalcResponse>(
      "calc",
      [](const std::shared_ptr<const firefly::test::CalcRequest>& req)
          -> firefly::ServiceResult<firefly::test::CalcResponse> {
        auto resp = std::make_shared<firefly::test::CalcResponse>();
        resp->set_sum(req->a() + req->b());
        return {firefly::ErrorCode::kOk, resp};
      }).value;
  if (!service) return 14;

  auto client = node->CreateClient<firefly::test::CalcRequest, firefly::test::CalcResponse>(
      "calc").value;
  if (!client) return 15;
  if (!client->WaitForService(std::chrono::seconds(2))) return 16;

  std::shared_ptr<firefly::test::CalcRequest> req =
      std::make_shared<firefly::test::CalcRequest>();
  req->set_a(10);
  req->set_b(32);
  auto resp = client->SendRequest(req, std::chrono::milliseconds(2000));
  if (!resp || resp.code != firefly::ErrorCode::kOk || resp->sum() != 42) return 17;

  // 寮傛璇锋眰
  auto fut = client->AsyncSendRequest(req);
  if (fut.future.get() == nullptr || fut.future.get()->sum() != 42) return 18;

  firefly::Clear();
  return 0;
}