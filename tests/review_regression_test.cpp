// review_regression_test.cpp：针对接口审查（main@4aecd32）列出的 7 项缺陷的
// 公开 API 回归。每项对应审查报告中的一条，确保修复不被后续改动回退。
#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <memory>
#include <string>
#include <thread>

#include "firefly/firefly.h"
#include "test_messages.pb.h"

using firefly::ErrorCode;
using firefly::test::CalcRequest;
using firefly::test::CalcResponse;
using firefly::test::Chatter;

static int Check(bool cond, int line) { return cond ? 0 : line; }

static std::shared_ptr<const CalcRequest> MakeReq(int a, int b) {
  auto r = std::make_shared<CalcRequest>();
  r->set_a(a);
  r->set_b(b);
  return r;
}

static Chatter MakeMsg(int seq) {
  Chatter m;
  m.set_seq(seq);
  return m;
}

// ---------- P1-1：释放最后一个 Service 引用后，服务应停止 ----------
static int TestServiceReleaseStops() {
  auto node = firefly::CreateNode("rv_svc_node").value;
  auto cb = [](const std::shared_ptr<const CalcRequest>&)
      -> firefly::ServiceResult<CalcResponse> {
    auto r = std::make_shared<CalcResponse>();
    r->set_sum(1);
    return {ErrorCode::kOk, r};
  };
  std::weak_ptr<firefly::ServiceBase> weak;
  {
    auto svc = node->CreateService<CalcRequest, CalcResponse>("rv_release_svc", cb).value;
    weak = svc;
    // 确认服务可见
    auto cli = node->CreateClient<CalcRequest, CalcResponse>("rv_release_svc").value;
    if (int e = Check(cli->WaitForService(std::chrono::seconds(2)), 40)) return e;
    cli->Shutdown();
    node->DeleteClient("rv_release_svc");
  }  // svc 释放最后一个外部引用 → 服务应停止并注销
  // core 不再被 worker 自持有，weak 应已过期
  if (int e = Check(weak.expired(), 41)) return e;
  // 注册中心不应再有该服务
  auto cli2 = node->CreateClient<CalcRequest, CalcResponse>("rv_release_svc").value;
  bool ready = cli2->WaitForService(std::chrono::milliseconds(300));
  if (int e = Check(!ready, 42)) return e;
  cli2->Shutdown();
  node->DeleteClient("rv_release_svc");
  return 0;
}

// ---------- P1-2：排队期间取消的 RPC 不应到达服务端 ----------
static int TestCancelBeforeSend() {
  auto node = firefly::CreateNode("rv_cancel_node").value;
  std::atomic<int> executed{0};
  std::atomic<bool> first_entered{false};
  // 慢服务：第一条请求处理 400ms，工作线程串行，制造排队窗口。
  auto svc = node->CreateService<CalcRequest, CalcResponse>(
      "rv_cancel_svc",
      [&](const std::shared_ptr<const CalcRequest>& req)
          -> firefly::ServiceResult<CalcResponse> {
        executed.fetch_add(1);
        if (req->a() == 1) first_entered.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        auto r = std::make_shared<CalcResponse>();
        r->set_sum(9);
        return {ErrorCode::kOk, r};
      }).value;
  auto cli = node->CreateClient<CalcRequest, CalcResponse>("rv_cancel_svc").value;
  if (int e = Check(cli->WaitForService(std::chrono::seconds(2)), 50)) return e;

  // 第一条请求占用连接（同步发送，后台线程持有 io_mutex_）。
  auto h1 = cli->AsyncSendRequest(MakeReq(1, 1), std::chrono::milliseconds(3000));
  // 等 h1 真正进入服务端回调（executed 已计 1），确保第二条会排队。
  // 这是"等待条件成立"而不是断言时长：给 3s 上限只为消除高负载下的启动抖动
  // （-j6 时 300ms 曾出现偶发失败），请求没送到服务端仍然会失败。
  for (int i = 0; i < 3000 && !first_entered.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (int e = Check(first_entered.load(), 51)) return e;
  const int before = executed.load();
  // 第二条请求排队，随即取消。
  auto h2 = cli->AsyncSendRequest(MakeReq(2, 2), std::chrono::milliseconds(3000));
  if (int e = Check(h2.Cancel(), 52)) return e;
  bool cancelled = false;
  try {
    (void)h2.future.get();
  } catch (const firefly::FireflyException& ex) {
    cancelled = (ex.code() == ErrorCode::kCancelled);
  }
  if (int e = Check(cancelled, 53)) return e;

  // 等第一条完成、队列清空后，服务端计数不应因取消的 h2 增加。
  (void)h1.future.get();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  if (int e = Check(executed.load() == before, 54)) return e;

  cli->Shutdown();
  node->DeleteClient("rv_cancel_svc");
  node->DeleteService("rv_cancel_svc");
  return 0;
}

// ---------- P1-3：DeleteReader 持锁等待回调导致死锁 ----------
static int TestDeleteReaderNoDeadlock() {
  auto node = firefly::CreateNode("rv_deadlock_node").value;
  std::atomic<bool> entered{false};
  std::atomic<bool> can_exit{false};
  // 回调内调用同一 Node 的 GetReader（需要 readers_mutex_）。
  auto reader = node->CreateReader<Chatter>(
      "rv_dl_ch",
      [&](const firefly::MessageContext&,
          const std::shared_ptr<const Chatter>&) {
        entered.store(true);
        (void)node->GetReader<Chatter>("rv_dl_ch");
        while (!can_exit.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return firefly::CallbackResult::kOk;
      }).value;
  auto writer = node->CreateWriter<Chatter>("rv_dl_ch").value;
  // 触发一条消息使回调进入
  writer->Write(MakeMsg(1));
  // 等回调进入
  for (int i = 0; i < 200 && !entered.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (int e = Check(entered.load(), 60)) return e;

  // 另一个线程调用 DeleteReader；它会等待回调返回。
  std::atomic<bool> deleted{false};
  std::thread([&]() {
    (void)node->DeleteReader("rv_dl_ch");
    deleted.store(true);
  }).detach();
  // 给删除线程充分时间进入等待；若死锁则 deleted 一直为 false。
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  if (int e = Check(!deleted.load(), 61)) return e;  // 此时应仍在等回调
  can_exit.store(true);
  // 轮询等待，带超时保护：万一回归为死锁，测试失败而非整体挂起。
  bool finished = false;
  for (int i = 0; i < 1000 && !finished; ++i) {
    finished = deleted.load();
    if (!finished) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (int e = Check(finished, 62)) return e;  // 回调退出后删除完成，未死锁
  writer->Shutdown();
  return 0;
}

// ---------- P1-4：RequestHandle 在 Client 析构后 Cancel 安全 ----------
static int TestRequestHandleAfterClientGone() {
  auto node = firefly::CreateNode("rv_handle_node").value;
  auto cb = [](const std::shared_ptr<const CalcRequest>&)
      -> firefly::ServiceResult<CalcResponse> {
    auto r = std::make_shared<CalcResponse>();
    r->set_sum(3);
    return {ErrorCode::kOk, r};
  };
  auto svc = node->CreateService<CalcRequest, CalcResponse>("rv_handle_svc", cb).value;
  using Handle = firefly::Client<CalcRequest, CalcResponse>::RequestHandle;
  Handle saved;
  {
    auto cli = node->CreateClient<CalcRequest, CalcResponse>("rv_handle_svc").value;
    if (int e = Check(cli->WaitForService(std::chrono::seconds(2)), 70)) return e;
    // 用一个尚未兑现的异步请求拿到句柄，随后让 Client 析构。
    auto slow = node->CreateService<CalcRequest, CalcResponse>(
        "rv_handle_slow",
        [](const std::shared_ptr<const CalcRequest>&)
            -> firefly::ServiceResult<CalcResponse> {
          std::this_thread::sleep_for(std::chrono::milliseconds(600));
          auto r = std::make_shared<CalcResponse>();
          r->set_sum(4);
          return {ErrorCode::kOk, r};
        }).value;
    auto cli2 = node->CreateClient<CalcRequest, CalcResponse>("rv_handle_slow").value;
    if (int e = Check(cli2->WaitForService(std::chrono::seconds(2)), 71)) return e;
    saved = cli2->AsyncSendRequest(MakeReq(1, 1), std::chrono::milliseconds(3000));
    if (int e = Check(saved.id != 0, 72)) return e;
    cli2->Shutdown();  // 在途请求被取消并兑现，core 随后释放
    node->DeleteClient("rv_handle_slow");
    node->DeleteService("rv_handle_slow");
  }
  // Client 已析构、core 已释放；Cancel 应安全返回 false，不访问悬空指针。
  if (int e = Check(saved.Cancel() == false, 73)) return e;
  node->DeleteClient("rv_handle_svc");
  node->DeleteService("rv_handle_svc");
  return 0;
}

// ---------- P1-5：删除一个订阅者不影响同通道其他订阅者 ----------
static int TestDeleteOneReaderKeepsOthers() {
  auto node1 = firefly::CreateNode("rv_sub_node1").value;
  auto node2 = firefly::CreateNode("rv_sub_node2").value;
  auto writer = node1->CreateWriter<Chatter>("rv_shared_ch").value;
  auto r1 = node1->CreateReader<Chatter>("rv_shared_ch").value;
  auto r2 = node2->CreateReader<Chatter>("rv_shared_ch").value;
  // 两个 Reader 在线：Writer 应能看到订阅者
  if (int e = Check(writer->HasReader(), 80)) return e;
  // 删除其中一个
  if (int e = Check(node1->DeleteReader("rv_shared_ch"), 81)) return e;
  // 另一个仍在线，Writer 不应误判为无订阅者
  if (int e = Check(writer->HasReader(), 82)) return e;
  if (int e = Check(writer->Write(MakeMsg(7)) == firefly::WriteStatus::kAccepted, 83)) return e;
  // 删掉最后一个才变为无订阅者
  if (int e = Check(node2->DeleteReader("rv_shared_ch"), 84)) return e;
  if (int e = Check(!writer->HasReader(), 85)) return e;
  writer->Shutdown();
  return 0;
}

// ---------- P2-7：Clear 后重启不残留旧发现记录 ----------
static int TestClearRestartNoStale() {
  {
    auto node = firefly::CreateNode("rv_clear_node1").value;
    auto w = node->CreateWriter<Chatter>("rv_restart_ch").value;
    auto r = node->CreateReader<Chatter>("rv_restart_ch").value;
    if (int e = Check(w->HasReader(), 90)) return e;  // 前置：确有 reader 记录
    // 关键顺序：先 Clear（Registry::Stop），再让 reader/node 析构注销。
    // 旧代码 Stop 不清表且停后 Unregister 被跳过 → 记录残留（审查 P2）。
    firefly::Clear();
  }  // w/r/node 在 Clear 之后析构
  firefly::Init("review_regression_restart");
  {
    auto node2 = firefly::CreateNode("rv_clear_node2").value;
    auto w2 = node2->CreateWriter<Chatter>("rv_restart_ch").value;
    // 重启后不应残留上一环节的 reader 记录
    if (int e = Check(!w2->HasReader(), 91)) return e;
    w2->Shutdown();
  }
  return 0;
}

int main() {
  if (int e = firefly::Init("review_regression_test") ? 0 : 1) return e;

  struct Case { const char* name; int (*fn)(); };
  Case cases[] = {
      {"service_release_stops",   TestServiceReleaseStops},
      {"cancel_before_send",      TestCancelBeforeSend},
      {"delete_reader_no_deadlock", TestDeleteReaderNoDeadlock},
      {"handle_after_client_gone", TestRequestHandleAfterClientGone},
      {"delete_one_reader_keeps_others", TestDeleteOneReaderKeepsOthers},
      {"clear_restart_no_stale",  TestClearRestartNoStale},
  };
  for (auto& c : cases) {
    if (int e = c.fn()) {
      std::printf("review_regression: %s FAILED at line %d\n", c.name, e);
      return e;
    }
  }
  firefly::Clear();
  std::printf("review_regression: all 6 cases passed\n");
  return 0;
}
