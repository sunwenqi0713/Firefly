// follower_reconcile_test.cpp：Follower 定期对账的删除缺口回归（G1）。
//
// 用一个只应答 QLIST、从不发广播的假主实例模拟两种边界：
//   delete     —— 主实例权威快照已为空（端点注销了，但删除广播丢了）。
//                 Follower 必须在下一轮对账（3 秒）后移除本地记录，否则
//                 Reader 会一直 dial 过期地址。
//   query-fail —— 主实例暂时不可达（查询超时）。这时的空结果是"没查到"，
//                 不是"没有记录"，Follower 必须保留本地缓存，等下一轮再试。
//
// 用法：follower_reconcile_test [delete|query-fail]
#include "backends/nng/registry.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nng/nng.h>
#include <nng/protocol/pubsub0/pub.h>
#include <nng/protocol/reqrep0/rep.h>

#include "backends/nng/net.h"

using namespace std::chrono_literals;
using firefly::detail::EndpointRecord;
using firefly::detail::ListenOnEphemeralPort;
using firefly::discovery::Kind;
using firefly::discovery::Registry;

namespace {

// 假主实例：绑定 REP/PUB 两个地址，只实现 QLIST；silent 时连请求都不取，
// 让 Follower 的查询超时失效，模拟"主实例不可达"。
class FakePrimary {
 public:
  ~FakePrimary() { Stop(); }

  bool Start() {
    if (nng_rep0_open(&rep_) != 0) return false;
    nng_setopt_ms(rep_, NNG_OPT_RECVTIMEO, 200);
    nng_setopt_ms(rep_, NNG_OPT_SENDTIMEO, 200);
    if (!ListenOnEphemeralPort(rep_, &rep_listener_, &rep_url_)) return false;
    if (nng_pub0_open(&pub_) != 0) return false;
    if (!ListenOnEphemeralPort(pub_, &pub_listener_, &pub_url_)) return false;
    thread_ = std::thread(&FakePrimary::Serve, this);
    return true;
  }

  void Stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    if (rep_.id != 0) nng_close(rep_);
    if (pub_.id != 0) nng_close(pub_);
    rep_ = NNG_SOCKET_INITIALIZER;
    pub_ = NNG_SOCKET_INITIALIZER;
  }

  // 设置 QLIST 返回的记录集合（id + 地址），backend 固定为 nng。
  void SetRecords(std::vector<std::pair<std::uint64_t, std::string>> records) {
    std::lock_guard<std::mutex> lock(mutex_);
    records_ = std::move(records);
  }

  // 进入/退出静默：静默期间收到的查询一律不回复。
  void SetSilent(bool silent) {
    std::lock_guard<std::mutex> lock(mutex_);
    silent_ = silent;
  }

  const std::string& rep_url() const { return rep_url_; }
  const std::string& pub_url() const { return pub_url_; }

 private:
  void Serve() {
    while (running_.load()) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (silent_) {
          // 不取请求即不回复；REQ 侧超时退化为查询失败。
          std::this_thread::sleep_for(50ms);
          continue;
        }
      }
      void* data = nullptr;
      std::size_t size = 0;
      int r = nng_recv(rep_, &data, &size, NNG_FLAG_ALLOC);
      if (r != 0) continue;  // 超时或对端已断开
      const std::string req(static_cast<char*>(data), size);
      nng_free(data, size);
      Send(Respond(req));
    }
  }

  std::string Respond(const std::string& req) {
    if (req.rfind("QLIST", 0) != 0) return "ERR unknown cmd\n";
    std::string out;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      out = "OK " + std::to_string(records_.size()) + "\n";
      for (const auto& rec : records_) {
        out += std::to_string(rec.first) + "\tnng\t" + rec.second + "\n";
      }
    }
    return out;
  }

  void Send(const std::string& msg) {
    nng_send(rep_, const_cast<char*>(msg.data()), msg.size(), 0);
  }

  nng_socket rep_{NNG_SOCKET_INITIALIZER};
  nng_socket pub_{NNG_SOCKET_INITIALIZER};
  nng_listener rep_listener_{NNG_LISTENER_INITIALIZER};
  nng_listener pub_listener_{NNG_LISTENER_INITIALIZER};
  std::string rep_url_;
  std::string pub_url_;
  std::atomic<bool> running_{true};
  std::mutex mutex_;
  std::vector<std::pair<std::uint64_t, std::string>> records_;
  bool silent_ = false;
  std::thread thread_;
};

// Watch 回调的观测点：最近一次快照、回调次数、是否出现过空快照。
class WatchProbe {
 public:
  void Update(const std::vector<EndpointRecord>& snapshot) {
    std::lock_guard<std::mutex> lock(mutex_);
    last_ = snapshot;
    ++callbacks_;
    if (snapshot.empty()) saw_empty_ = true;
  }

  std::size_t Size() {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_.size();
  }

  int Callbacks() {
    std::lock_guard<std::mutex> lock(mutex_);
    return callbacks_;
  }

  bool SawEmpty() {
    std::lock_guard<std::mutex> lock(mutex_);
    return saw_empty_;
  }

  bool WaitSize(std::size_t expected, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
      if (Size() == expected) return true;
      std::this_thread::sleep_for(20ms);
    }
    return Size() == expected;
  }

 private:
  std::mutex mutex_;
  std::vector<EndpointRecord> last_;
  int callbacks_ = 0;
  bool saw_empty_ = false;
};

}  // namespace

int main(int argc, char** argv) {
  const std::string mode = argc > 1 ? argv[1] : "delete";

  FakePrimary primary;
  if (!primary.Start()) return 1;
  primary.SetRecords({{7, "tcp://127.0.0.1:40901"}});

  Registry::Configure(primary.rep_url(), primary.pub_url());
  Registry& reg = Registry::instance();
  if (!reg.Start()) return 2;

  WatchProbe probe;
  const int watch = reg.Watch(
      Kind::kWriter, "lost-delete",
      [&probe](const std::vector<EndpointRecord>& snapshot) {
        probe.Update(snapshot);
      });
  if (watch == 0) return 3;

  // 初始快照必须来自主实例的主动查询（本地只有空缓存）。
  if (!probe.WaitSize(1, 5000ms)) return 4;

  if (mode == "delete") {
    // 端点已注销、删除广播丢失：主实例的权威快照为空。
    primary.SetRecords({});
    // 对账周期 3 秒；留出查询开销余量。
    if (!probe.WaitSize(0, 10000ms)) return 5;
    // 记录移除后再来一轮空快照不应继续派发（集合未变）。
    const int before = probe.Callbacks();
    std::this_thread::sleep_for(4500ms);
    if (probe.Callbacks() != before) return 6;
    if (probe.Size() != 0) return 7;
  } else if (mode == "query-fail") {
    primary.SetSilent(true);
    // 覆盖至少一轮对账：查询失败不得清空本地记录。
    std::this_thread::sleep_for(5000ms);
    if (probe.SawEmpty()) return 8;
    if (probe.Size() != 1) return 9;
  } else {
    return 10;
  }

  reg.Unwatch(watch);
  reg.Stop();
  primary.Stop();
  return 0;
}
