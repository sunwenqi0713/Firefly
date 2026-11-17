// duplicate_test.cpp：CreateNode 进程内查重 + 单 Node 内 Writer/Service/Client 查重。
#include <string>

#include "firefly/firefly.h"
#include "test_messages.pb.h"

using firefly::ErrorCode;
using firefly::test::CalcRequest;
using firefly::test::CalcResponse;
using firefly::test::Chatter;

static int Check(bool cond, int line) { return cond ? 0 : line; }

int main() {
  firefly::Init("duplicate_test");

  // CreateNode 进程内查重
  {
    auto n1 = firefly::CreateNode("dup_node");
    if (int e = Check(static_cast<bool>(n1), 10)) return e;
    auto n2 = firefly::CreateNode("dup_node");
    if (int e = Check(!n2 && n2.code == ErrorCode::kDuplicate &&
                          !n2.message.empty(), 11)) return e;
    // 非法名
    auto n3 = firefly::CreateNode("");
    if (int e = Check(!n3 && n3.code == ErrorCode::kInvalidName, 12)) return e;
    // n1 析构后名字回收，可重建
  }
  auto n_re = firefly::CreateNode("dup_node");
  if (int e = Check(static_cast<bool>(n_re), 13)) return e;

  auto node = firefly::CreateNode("dup_endpoint_node").value;

  // Writer 单 Node 内查重
  auto w1 = node->CreateWriter<Chatter>("dup_ch");
  if (int e = Check(static_cast<bool>(w1), 20)) return e;
  auto w2 = node->CreateWriter<Chatter>("dup_ch");
  if (int e = Check(!w2 && w2.code == ErrorCode::kDuplicate &&
                        w2.message.find("dup_ch") != std::string::npos, 21)) return e;
  // 不同通道名不冲突
  auto w3 = node->CreateWriter<Chatter>("dup_ch_other");
  if (int e = Check(static_cast<bool>(w3), 22)) return e;

  // Service 单 Node 内查重
  auto cb = [](const std::shared_ptr<const CalcRequest>&)
      -> firefly::ServiceResult<CalcResponse> {
    auto r = std::make_shared<CalcResponse>();
    r->set_sum(1);
    return {ErrorCode::kOk, r};
  };
  auto s1 = node->CreateService<CalcRequest, CalcResponse>("dup_svc", cb);
  if (int e = Check(static_cast<bool>(s1), 30)) return e;
  auto s2 = node->CreateService<CalcRequest, CalcResponse>("dup_svc", cb);
  if (int e = Check(!s2 && s2.code == ErrorCode::kDuplicate, 31)) return e;

  // Client 单 Node 内查重
  auto c1 = node->CreateClient<CalcRequest, CalcResponse>("dup_svc");
  if (int e = Check(static_cast<bool>(c1), 40)) return e;
  auto c2 = node->CreateClient<CalcRequest, CalcResponse>("dup_svc");
  if (int e = Check(!c2 && c2.code == ErrorCode::kDuplicate, 41)) return e;

  // Reader 现有约束回归：同节点同通道重复
  auto r1 = node->CreateReader<Chatter>("dup_read_ch");
  if (int e = Check(static_cast<bool>(r1), 50)) return e;
  auto r2 = node->CreateReader<Chatter>("dup_read_ch");
  if (int e = Check(!r2 && r2.code == ErrorCode::kDuplicate, 51)) return e;

  firefly::Clear();
  return 0;
}
