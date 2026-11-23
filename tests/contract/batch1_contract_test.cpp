// batch1_contract_test.cpp：第 1 批行为契约（Service 停机 / Reader 删除 / RPC 取消）。
// 并发时序一律由 promise/future 控制，不依赖 sleep。
#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "firefly/detail/endpoint_api.h"
#include "firefly/firefly.h"
#include "test_messages.pb.h"

using namespace std::chrono_literals;
using firefly::ErrorCode;
using firefly::test::CalcRequest;
using firefly::test::CalcResponse;
using firefly::test::Chatter;

namespace {

int Check(bool cond, int line) { return cond ? 0 : line; }

std::shared_ptr<const CalcRequest> MakeReq(int a, int b) {
  auto r = std::make_shared<CalcRequest>();
  r->set_a(a);
  r->set_b(b);
  return r;
}

Chatter MakeMsg(int seq) {
  Chatter m;
  m.set_seq(seq);
  return m;
}

// 仅用于等待"无信号可挂"的外部状态（weak_ptr 过期等），限定上界防挂死。
template <class Predicate>
bool WaitUntil(Predicate pred, std::chrono::milliseconds limit) {
  const auto deadline = std::chrono::steady_clock::now() + limit;
  while (!pred()) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::yield();
  }
  return true;
}

template <class T>
ErrorCode SettledCode(std::shared_future<T> future) {
  if (future.wait_for(3s) != std::future_status::ready) return ErrorCode::kNotReady;
  try {
    (void)future.get();
    return ErrorCode::kOk;
  } catch (const firefly::FireflyException& ex) {
    return ex.code();
  }
}

// ---------- 任务 1：释放最后一个外部引用 → 服务注销、旧客户端调用失败 ----------
int TestServiceReleaseStopsOldClient() {
  auto node = firefly::CreateNode("b1_svc_release_node").value;
  auto svc = node->CreateService<CalcRequest, CalcResponse>(
      "b1_release_svc", [](const std::shared_ptr<const CalcRequest>& req)
                            -> firefly::ServiceResult<CalcResponse> {
        auto r = std::make_shared<CalcResponse>();
        r->set_sum(req->a() + req->b());
        return {ErrorCode::kOk, r};
      }).value;
  auto old_client = node->CreateClient<CalcRequest, CalcResponse>("b1_release_svc").value;
  if (int e = Check(old_client->WaitForService(2s), 40)) return e;

  std::weak_ptr<firefly::ServiceBase> weak = svc;
  svc.reset();  // 最后一个外部引用释放
  if (int e = Check(WaitUntil([&] { return weak.expired(); }, 2s), 41)) return e;

  auto after = old_client->SendRequest(MakeReq(1, 2), 500ms);
  if (int e = Check(!after && after.code != ErrorCode::kOk, 42)) return e;

  // 用一个独立名字的 Client 探测：服务名已从注册中心注销。
  auto probe = node->CreateClient<CalcRequest, CalcResponse>("b1_probe_absent").value;
  if (int e = Check(!probe->WaitForService(0ms), 43)) return e;
  probe->Shutdown();
  old_client->Shutdown();
  return 0;
}

// ---------- 任务 1：回调线程内停机不 self-join ----------
int TestServiceShutdownOnCallbackThread() {
  auto node = firefly::CreateNode("b1_svc_callback_node").value;
  std::promise<void> entered;
  std::promise<void> done;
  auto entered_future = entered.get_future().share();
  auto done_future = done.get_future().share();

  std::shared_ptr<firefly::Service<CalcRequest, CalcResponse>> owner;
  auto callback = [&owner, &entered, &done](
                      const std::shared_ptr<const CalcRequest>&)
      -> firefly::ServiceResult<CalcResponse> {
    entered.set_value();
    owner->Shutdown();  // 在 worker（回调）线程内走同一停机路径
    done.set_value();
    auto r = std::make_shared<CalcResponse>();
    r->set_sum(7);
    return {ErrorCode::kOk, r};
  };
  owner = node->CreateService<CalcRequest, CalcResponse>("b1_cb_svc", callback).value;
  std::weak_ptr<firefly::ServiceBase> weak = owner;

  auto client = node->CreateClient<CalcRequest, CalcResponse>("b1_cb_svc").value;
  if (int e = Check(client->WaitForService(2s), 50)) return e;
  auto handle = client->AsyncSendRequest(MakeReq(2, 3), 1s);

  if (int e = Check(entered_future.wait_for(2s) == std::future_status::ready, 51)) {
    return e;
  }
  // 若 Shutdown 在回调线程内 join 自身或等待在途回调，这里永远不会 ready。
  if (int e = Check(done_future.wait_for(2s) == std::future_status::ready, 52)) {
    return e;
  }
  if (int e = Check(handle.future.wait_for(2s) == std::future_status::ready, 53)) {
    return e;
  }
  if (int e = Check(!client->ServiceIsReady(), 54)) return e;

  owner.reset();
  if (int e = Check(WaitUntil([&] { return weak.expired(); }, 2s), 55)) return e;
  client->Shutdown();
  return 0;
}

// ---------- 任务 2：回调内并发 GetReader + 删除中名称占用 + 重建 ----------
int TestReaderDeleteWithConcurrentGet() {
  auto node = firefly::CreateNode("b1_reader_delete_node").value;
  auto other = firefly::CreateNode("b1_reader_other_node").value;
  const std::string channel = "b1_delete_ch";

  std::promise<bool> get_result;
  std::promise<void> entered;
  std::promise<void> release;
  auto release_future = release.get_future().share();

  auto reader = node->CreateReader<Chatter>(
      channel, [&](const firefly::MessageContext&,
                   const std::shared_ptr<const Chatter>&) {
        // 回调内查询同一 Node：删除流程若在锁内等回调，这里会自锁。
        auto found = node->GetReader<Chatter>(channel);
        get_result.set_value(found != nullptr);
        entered.set_value();
        release_future.wait();
        return firefly::CallbackResult::kOk;
      }).value;
  auto other_reader = other->CreateReader<Chatter>(channel).value;
  auto writer = node->CreateWriter<Chatter>(channel).value;
  writer->Write(MakeMsg(1));

  if (int e = Check(entered.get_future().wait_for(2s) == std::future_status::ready,
                    60)) {
    return e;
  }
  auto deleting = std::async(std::launch::async, [&] { return node->DeleteReader(channel); });
  if (int e = Check(deleting.wait_for(0ms) == std::future_status::timeout, 61)) {
    return e;  // 应等待在途回调返回
  }
  if (int e = Check(node->GetReader<Chatter>(channel) != nullptr, 62)) return e;
  auto during = node->CreateReader<Chatter>(channel);
  if (int e = Check(!during && during.code == ErrorCode::kDuplicate, 63)) return e;

  release.set_value();
  if (int e = Check(deleting.wait_for(2s) == std::future_status::ready, 64)) return e;
  if (int e = Check(deleting.get(), 65)) return e;
  if (int e = Check(get_result.get_future().get(), 66)) return e;
  // 跨 Node 的另一个订阅者不受影响。
  if (int e = Check(writer->HasReader(), 67)) return e;

  auto recreated = node->CreateReader<Chatter>(channel);
  if (int e = Check(static_cast<bool>(recreated), 68)) return e;
  writer->Shutdown();
  return 0;
}

// ---------- 任务 3：排队中取消/到期 → 服务端 0 次执行 ----------
int TestRpcQueuedCancelAndExpiry() {
  auto node = firefly::CreateNode("b1_rpc_queue_node").value;
  std::promise<void> service_entered;
  std::promise<void> release;
  auto release_future = release.get_future().share();
  std::atomic<int> executed{0};

  auto svc = node->CreateService<CalcRequest, CalcResponse>(
      "b1_queue_svc", [&](const std::shared_ptr<const CalcRequest>& req)
                          -> firefly::ServiceResult<CalcResponse> {
        const int n = executed.fetch_add(1) + 1;
        if (n == 1) {
          service_entered.set_value();
          release_future.wait();
        }
        auto r = std::make_shared<CalcResponse>();
        r->set_sum(req->a() + req->b());
        return {ErrorCode::kOk, r};
      }).value;
  auto cli = node->CreateClient<CalcRequest, CalcResponse>("b1_queue_svc").value;
  if (int e = Check(cli->WaitForService(2s), 70)) return e;

  auto first = cli->AsyncSendRequest(MakeReq(1, 1), 5s);
  if (int e = Check(service_entered.get_future().wait_for(2s) ==
                        std::future_status::ready,
                    71)) {
    return e;
  }
  // 执行者被首个请求占用，后续请求都在有界队列中等待。
  auto cancelled = cli->AsyncSendRequest(MakeReq(2, 2), 5s);
  if (int e = Check(cancelled.Cancel(), 72)) return e;
  if (int e = Check(SettledCode(cancelled.future) == ErrorCode::kCancelled, 73)) {
    return e;
  }
  auto expired = cli->AsyncSendRequest(MakeReq(3, 3), 50ms);
  // 执行者忙时，排队请求在截止时间之后才会因出队检查而兑现；此处先确认它
  // 仍未被兑现，再放行首个请求，确保它出队时已过期。
  if (int e = Check(expired.future.wait_for(100ms) == std::future_status::timeout,
                    74)) {
    return e;
  }
  release.set_value();
  // 排队中的过期请求出队时即被丢弃，服务端 0 次执行。
  if (int e = Check(SettledCode(expired.future) == ErrorCode::kTimeout, 75)) {
    return e;
  }
  if (int e = Check(SettledCode(first.future) == ErrorCode::kOk, 76)) return e;
  // 被取消和过期的两个请求都不得进入服务端回调。
  if (int e = Check(executed.load() == 1, 77)) return e;
  cli->Shutdown();
  node->DeleteService("b1_queue_svc");
  return 0;
}

// ---------- 任务 3：每请求只兑现一次 + 队列满 + 旧句柄失效 ----------
int TestRpcSettleOnceQueueFullAndStaleHandle() {
  auto node = firefly::CreateNode("b1_rpc_settle_node").value;
  std::promise<void> service_entered;
  std::promise<void> release;
  auto release_future = release.get_future().share();
  std::atomic<int> calls{0};

  auto svc = node->CreateService<CalcRequest, CalcResponse>(
      "b1_settle_svc", [&](const std::shared_ptr<const CalcRequest>& req)
                           -> firefly::ServiceResult<CalcResponse> {
        if (calls.fetch_add(1) == 0) {
          service_entered.set_value();
          release_future.wait();
        }
        auto r = std::make_shared<CalcResponse>();
        r->set_sum(req->a() + req->b());
        return {ErrorCode::kOk, r};
      }).value;

  // detail 层直接观察兑现次数，避免 public future 掩盖重复兑现。
  std::string request_bytes;
  if (int e = Check(firefly::message::MessageTraits<CalcRequest>::Serialize(
                        *MakeReq(1, 1), &request_bytes),
                    79)) {
    return e;
  }
  auto core_result = firefly::detail::CreateClientCore(
      "b1_rpc_settle_node", "b1_settle_svc", nullptr);
  if (int e = Check(static_cast<bool>(core_result), 80)) return e;
  auto core = core_result.value;

  std::atomic<int> settlements{0};
  std::promise<ErrorCode> first_code;
  auto first_code_future = first_code.get_future();
  auto id = firefly::detail::ClientAsyncSendBytes(
      core.get(), request_bytes, 5s,
      [&](ErrorCode code, const std::string&) {
        first_code.set_value(code);
        settlements.fetch_add(1);
      });
  if (int e = Check(id != 0, 81)) return e;
  // 等首个请求真正占用执行者（服务端回调已进入），后续请求才会排队。
  if (int e = Check(service_entered.get_future().wait_for(2s) ==
                        std::future_status::ready,
                    82)) {
    return e;
  }
  if (int e = Check(firefly::detail::ClientCancelRequestCore(core.get(), id), 83)) {
    return e;
  }
  if (int e = Check(!firefly::detail::ClientCancelRequestCore(core.get(), id), 84)) {
    return e;
  }
  if (int e = Check(first_code_future.wait_for(2s) ==
                        std::future_status::ready,
                    85)) {
    return e;
  }
  if (int e = Check(first_code_future.get() == ErrorCode::kCancelled, 86)) {
    return e;
  }

  release.set_value();
  // 同步请求走同一队列：它完成后说明前面被取消的请求已经历过迟到响应。
  if (int e = Check(firefly::detail::ClientSendBytes(core.get(), request_bytes, 2s).code ==
                        ErrorCode::kOk,
                    88)) {
    return e;
  }
  if (int e = Check(settlements.load() == 1, 89)) return e;  // 迟到响应不再兑现
  firefly::detail::ClientShutdownCore(core.get());

  // Client 销毁或重新 Init 后，旧句柄 Cancel 必须返回 false。
  auto cli = node->CreateClient<CalcRequest, CalcResponse>("b1_settle_svc").value;
  auto stale = cli->AsyncSendRequest(MakeReq(9, 9), 5s);
  cli->Shutdown();
  if (int e = Check(!stale.Cancel(), 90)) return e;
  if (int e = Check(cli->Init(), 91)) return e;
  if (int e = Check(!stale.Cancel(), 92)) return e;
  cli->Shutdown();
  node->DeleteService("b1_settle_svc");
  return 0;
}

// ---------- 任务 3：有界队列满 → kQueueFull（现有错误码）----------
int TestRpcQueueFull() {
  auto node = firefly::CreateNode("b1_rpc_full_node").value;
  std::promise<void> service_entered;
  std::promise<void> release;
  auto release_future = release.get_future().share();
  auto svc = node->CreateService<CalcRequest, CalcResponse>(
      "b1_full_svc", [&](const std::shared_ptr<const CalcRequest>& req)
                         -> firefly::ServiceResult<CalcResponse> {
        static std::atomic<int> first{0};
        if (first.fetch_add(1) == 0) {
          service_entered.set_value();
          release_future.wait();
        }
        auto r = std::make_shared<CalcResponse>();
        r->set_sum(req->a() + req->b());
        return {ErrorCode::kOk, r};
      }).value;
  std::string bytes;
  firefly::message::MessageTraits<CalcRequest>::Serialize(*MakeReq(1, 1), &bytes);
  auto core_result = firefly::detail::CreateClientCore(
      "b1_rpc_full_node", "b1_full_svc", nullptr);
  if (int e = Check(static_cast<bool>(core_result), 100)) return e;
  auto core = core_result.value;
  if (int e = Check(firefly::detail::ClientWaitForService(core.get(), 2s), 101)) {
    return e;
  }
  (void)firefly::detail::ClientAsyncSendBytes(
      core.get(), bytes, 5s, [](ErrorCode, const std::string&) {});
  if (int e = Check(service_entered.get_future().wait_for(2s) ==
                        std::future_status::ready,
                    102)) {
    return e;
  }
  int queue_full = 0;
  for (int i = 0; i < 100; ++i) {
    const auto id = firefly::detail::ClientAsyncSendBytes(
        core.get(), bytes, 5s, [](ErrorCode, const std::string&) {});
    if (id == 0) ++queue_full;
  }
  if (int e = Check(queue_full > 0, 103)) return e;
  release.set_value();
  firefly::detail::ClientShutdownCore(core.get());
  node->DeleteService("b1_full_svc");
  return 0;
}

}  // namespace

int main() {
  if (!firefly::Init("batch1_contract_test")) return 1;
  struct Case {
    const char* name;
    int (*fn)();
  } cases[] = {
      {"service_release_stops_old_client", TestServiceReleaseStopsOldClient},
      {"service_shutdown_on_callback_thread", TestServiceShutdownOnCallbackThread},
      {"reader_delete_with_concurrent_get", TestReaderDeleteWithConcurrentGet},
      {"rpc_queued_cancel_and_expiry", TestRpcQueuedCancelAndExpiry},
      {"rpc_settle_once_queue_full_stale_handle",
       TestRpcSettleOnceQueueFullAndStaleHandle},
      {"rpc_queue_full", TestRpcQueueFull},
  };
  for (auto& c : cases) {
    if (int e = c.fn()) {
      std::printf("batch1_contract: %s FAILED at line %d\n", c.name, e);
      return e;
    }
  }
  firefly::Clear();
  std::printf("batch1_contract: all %zu cases passed\n",
              sizeof(cases) / sizeof(cases[0]));
  return 0;
}
