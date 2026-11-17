// shutdown_test.cpp：全局关停 API 与生命周期语义（阶段二契约缺失 10）。
#include <chrono>
#include <string>

#include "firefly/firefly.h"
#include "test_messages.pb.h"

using firefly::ErrorCode;
using firefly::test::Chatter;

static int Check(bool cond, int line) {
  return cond ? 0 : line;
}

int main() {
  firefly::Init("shutdown_test");

  // 未关停：带超时等待应超时（false），OK()==true，IsShutdown()==false
  if (int e = Check(firefly::WaitForShutdownFor(std::chrono::milliseconds(50)) == false, 10)) return e;
  if (int e = Check(firefly::OK(), 11)) return e;
  if (int e = Check(!firefly::IsShutdown(), 12)) return e;

  // 主动关停：WaitForShutdownFor 应立即返回 true；OK() 变 false
  firefly::Shutdown();
  if (int e = Check(firefly::IsShutdown(), 20)) return e;
  if (int e = Check(!firefly::OK(), 21)) return e;
  if (int e = Check(firefly::WaitForShutdownFor(std::chrono::milliseconds(100)), 22)) return e;
  firefly::WaitForShutdown();  // 立即返回，不阻塞

  // 关停后 Create* 工厂返回 kNotReady（复用阶段一 OK() 状态位）
  auto node = firefly::CreateNode("shutdown_test_node").value;
  auto w = node->CreateWriter<Chatter>("ch_after_shutdown");
  if (int e = Check(!w && w.code == ErrorCode::kNotReady && !w.message.empty(), 30)) return e;
  auto svc = node->CreateService<firefly::test::CalcRequest, firefly::test::CalcResponse>(
      "svc_after_shutdown",
      [](const std::shared_ptr<const firefly::test::CalcRequest>&)
          -> firefly::ServiceResult<firefly::test::CalcResponse> {
        return {ErrorCode::kServiceFailed, nullptr};
      });
  if (int e = Check(!svc && svc.code == ErrorCode::kNotReady, 31)) return e;

  firefly::Clear();
  return 0;
}
