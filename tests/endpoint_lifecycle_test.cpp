// endpoint_lifecycle_test.cpp：DeleteXxx + Node 所有权契约（阶段二任务3）。
#include <chrono>
#include <memory>
#include <string>

#include "firefly/firefly.h"
#include "test_messages.pb.h"

using firefly::ErrorCode;
using firefly::test::CalcRequest;
using firefly::test::CalcResponse;
using firefly::test::Chatter;

static int Check(bool cond, int line) { return cond ? 0 : line; }
static bool Ok(const firefly::Result<void>& r) { return static_cast<bool>(r); }

static Chatter MakeMsg(int seq) {
  Chatter m;
  m.set_seq(seq);
  return m;
}

int main() {
  firefly::Init("endpoint_lifecycle_test");
  auto node = firefly::CreateNode("lifecycle_node").value;

  // DeleteWriter：删除后再 Write 返回 kTransportError（WriteStatus 折叠码），
  // 名字可回收，再 CreateWriter 成功。
  Chatter msg = MakeMsg(1);
  {
    auto w = node->CreateWriter<Chatter>("lc_ch").value;
    if (int e = Check(static_cast<bool>(w), 10)) return e;
    if (int e = Check(Ok(node->DeleteWriter("lc_ch")), 11)) return e;
    if (int e = Check(w->Write(msg) == firefly::WriteStatus::kTransportError, 12)) return e;
    if (int e = Check(!Ok(node->DeleteWriter("lc_ch")), 13)) return e;  // 重复删除失败
    auto w2 = node->CreateWriter<Chatter>("lc_ch");
    if (int e = Check(static_cast<bool>(w2), 14)) return e;
  }

  // DeleteClient：删除后再 SendRequest 返回 kNotReady，名字可回收。
  auto cb = [](const std::shared_ptr<const CalcRequest>&)
      -> firefly::ServiceResult<CalcResponse> {
    auto r = std::make_shared<CalcResponse>();
    r->set_sum(7);
    return {ErrorCode::kOk, r};
  };
  auto svc = node->CreateService<CalcRequest, CalcResponse>("lc_svc", cb).value;
  {
    auto cli = node->CreateClient<CalcRequest, CalcResponse>("lc_svc").value;
    if (int e = Check(static_cast<bool>(cli), 20)) return e;
    CalcRequest req;
    req.set_a(3); req.set_b(4);
    auto ok = cli->SendRequest(req, std::chrono::milliseconds(1000));
    if (int e = Check(static_cast<bool>(ok) && ok->sum() == 7, 21)) return e;
    if (int e = Check(Ok(node->DeleteClient("lc_svc")), 22)) return e;
    auto after = cli->SendRequest(req, std::chrono::milliseconds(500));
    if (int e = Check(!after && after.code == ErrorCode::kNotReady, 23)) return e;
    auto cli2 = node->CreateClient<CalcRequest, CalcResponse>("lc_svc");
    if (int e = Check(static_cast<bool>(cli2), 24)) return e;
  }

  // DeleteService：删除后注册中心不再有该服务，名字可回收。
  {
    if (int e = Check(Ok(node->DeleteService("lc_svc")), 30)) return e;
    auto svc2 = node->CreateService<CalcRequest, CalcResponse>("lc_svc", cb);
    if (int e = Check(static_cast<bool>(svc2), 31)) return e;
  }

  // Node 所有权契约（方案 b）：Node 析构不关闭 Writer/Service/Client，
  // 端点由外部 shared_ptr 独立存活，析构后仍可用。
  std::shared_ptr<firefly::Writer<Chatter>> surviving_writer;
  {
    auto tmp_node = firefly::CreateNode("lifecycle_tmp_node").value;
    surviving_writer = tmp_node->CreateWriter<Chatter>("lc_survive_ch").value;
    // Node 析构（离开作用域）
  }
  if (int e = Check(surviving_writer != nullptr && surviving_writer->IsInit(), 40)) return e;
  // 无订阅者时 Write 返回 kNoSubscriber（端点仍存活，非 kNotReady）
  if (int e = Check(surviving_writer->Write(MakeMsg(2)) ==
                        firefly::WriteStatus::kNoSubscriber, 41)) return e;
  surviving_writer->Shutdown();

  firefly::Clear();
  return 0;
}
