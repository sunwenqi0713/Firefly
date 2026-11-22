#include "firefly/detail/interfaces.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>

#include <nng/nng.h>
#include <nng/protocol/pubsub0/pub.h>
#include <nng/protocol/pubsub0/sub.h>
#include <nng/protocol/reqrep0/rep.h>
#include <nng/protocol/reqrep0/req.h>

#include "firefly/common/log.h"
#include "firefly/detail/discovery.h"
#include "firefly/detail/net.h"

namespace firefly {
namespace detail {
namespace {

// NNG socket 的 RAII 句柄：只由四个角色适配器在内部使用，不再是通用 ISocket。
class NngSocket final {
 public:
  ~NngSocket() { Close(); }

  nng_socket& Socket() noexcept { return sock_; }

  void Close() {
    if (sock_.id != 0) {
      nng_close(sock_);
      sock_.id = 0;
    }
  }

 private:
  nng_socket sock_{0};
};

// 绝对截止时间 → 正整数毫秒；每个阻塞点之前都重新计算，绝不复用旧预算。
int RemainingMs(Deadline deadline) noexcept {
  const auto now = std::chrono::steady_clock::now();
  if (now >= deadline) return 0;
  const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                      deadline - now)
                      .count();
  const auto ms = (us + 999) / 1000;
  if (ms > std::numeric_limits<int>::max()) return std::numeric_limits<int>::max();
  return static_cast<int>(ms);
}

TransferStatus MapNngStatus(int r) noexcept {
  if (r == 0) return TransferStatus::kOk;
  if (r == NNG_EAGAIN) return TransferStatus::kWouldBlock;
  if (r == NNG_ETIMEDOUT) return TransferStatus::kTimeout;
  if (r == NNG_ECLOSED || r == NNG_ECANCELED) return TransferStatus::kClosed;
  if (r == NNG_EINVAL) return TransferStatus::kInvalidArgument;
  return TransferStatus::kIoError;
}

// ---------------- Publisher / Subscriber（T1） ----------------

class NngPublisher final : public Publisher {
 public:
  explicit NngPublisher(std::unique_ptr<NngSocket> sock) : sock_(std::move(sock)) {}

  TransferStatus Bind(std::string* endpoint) override {
    if (endpoint == nullptr) return TransferStatus::kInvalidArgument;
    if (closed_.load(std::memory_order_acquire)) return TransferStatus::kClosed;
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (closed_.load(std::memory_order_acquire)) return TransferStatus::kClosed;
    nng_listener listener = NNG_LISTENER_INITIALIZER;
    if (!ListenOnEphemeralPort(sock_->Socket(), &listener, endpoint)) {
      return TransferStatus::kIoError;
    }
    return TransferStatus::kOk;
  }

  TransferStatus Send(const std::string& bytes) override {
    if (closed_.load(std::memory_order_acquire)) return TransferStatus::kClosed;
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (closed_.load(std::memory_order_acquire)) return TransferStatus::kClosed;
    int r = nng_send(sock_->Socket(), const_cast<char*>(bytes.data()),
                     bytes.size(), NNG_FLAG_NONBLOCK);
    return MapNngStatus(r);
  }

  void Close() override {
    if (closed_.exchange(true, std::memory_order_acq_rel)) return;
    std::lock_guard<std::mutex> lock(io_mutex_);
    sock_->Close();
  }

 private:
  std::unique_ptr<NngSocket> sock_;
  std::mutex io_mutex_;
  std::atomic<bool> closed_{false};
};

class NngSubscriber final : public Subscriber {
 public:
  explicit NngSubscriber(std::unique_ptr<NngSocket> sock) : sock_(std::move(sock)) {}

  TransferStatus UpdatePublishers(
      const std::vector<std::string>& endpoints) override {
    if (closed_.load(std::memory_order_acquire)) return TransferStatus::kClosed;
    std::vector<std::string> desired;
    for (const auto& endpoint : endpoints) {
      if (!endpoint.empty()) desired.push_back(endpoint);
    }
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (closed_.load(std::memory_order_acquire)) return TransferStatus::kClosed;
    for (auto it = dialers_.begin(); it != dialers_.end();) {
      if (std::find(desired.begin(), desired.end(), it->first) ==
          desired.end()) {
        nng_dialer_close(it->second);
        it = dialers_.erase(it);
      } else {
        ++it;
      }
    }
    for (const auto& endpoint : desired) {
      if (dialers_.count(endpoint) != 0) continue;
      nng_dialer dialer = NNG_DIALER_INITIALIZER;
      int r = nng_dial(sock_->Socket(), endpoint.c_str(), &dialer, 0);
      if (r != 0) return MapNngStatus(r);
      dialers_[endpoint] = dialer;
    }
    return TransferStatus::kOk;
  }

  TransferStatus Receive(std::string* bytes, Deadline deadline) override {
    if (bytes == nullptr) return TransferStatus::kInvalidArgument;
    for (;;) {
      if (closed_.load(std::memory_order_acquire)) return TransferStatus::kClosed;
      const int left = RemainingMs(deadline);
      if (left <= 0) return TransferStatus::kTimeout;
      void* data = nullptr;
      size_t size = 0;
      int r = 0;
      {
        std::lock_guard<std::mutex> lock(io_mutex_);
        if (closed_.load(std::memory_order_acquire)) return TransferStatus::kClosed;
        r = nng_recv(sock_->Socket(), &data, &size,
                     NNG_FLAG_ALLOC | NNG_FLAG_NONBLOCK);
      }
      if (r == 0) {
        bytes->assign(static_cast<char*>(data), size);
        nng_free(data, size);
        return TransferStatus::kOk;
      }
      TransferStatus status = MapNngStatus(r);
      if (status == TransferStatus::kWouldBlock) {
        // 非阻塞轮询 + 条件变量等待：Close() 可立刻唤醒，且不需要 sleep 固定时长。
        std::unique_lock<std::mutex> wait_lock(wait_mutex_);
        cv_.wait_for(wait_lock,
                     std::chrono::milliseconds(std::min(left, 20)),
                     [this] { return closed_.load(std::memory_order_acquire); });
        continue;
      }
      return status;
    }
  }

  void Close() override {
    if (closed_.exchange(true, std::memory_order_acq_rel)) return;
    {
      std::lock_guard<std::mutex> lock(io_mutex_);
      for (auto& kv : dialers_) nng_dialer_close(kv.second);
      dialers_.clear();
      sock_->Close();
    }
    cv_.notify_all();
  }

 private:
  std::unique_ptr<NngSocket> sock_;
  std::mutex io_mutex_;
  std::map<std::string, nng_dialer> dialers_;  // 受 io_mutex_ 保护
  std::mutex wait_mutex_;
  std::condition_variable cv_;
  std::atomic<bool> closed_{false};
};

// ---------------- RpcRequester / RpcResponder（T3） ----------------

// 适配器私有的不透明 token：Core 只能看到 ReplyToken 的虚析构接口。
class NngReplyToken final : public ReplyToken {
 public:
  NngReplyToken(const void* owner, std::uint64_t sequence)
      : owner_(owner), sequence_(sequence) {}
  const void* Owner() const noexcept { return owner_; }
  std::uint64_t Sequence() const noexcept { return sequence_; }

 private:
  const void* owner_;
  std::uint64_t sequence_;
};

void WaitSlice(std::condition_variable& cv, std::unique_lock<std::mutex>& lock,
               Deadline deadline, const std::atomic<bool>& closed) {
  const int left = RemainingMs(deadline);
  if (left <= 0) return;
  cv.wait_for(lock, std::chrono::milliseconds(std::min(left, 20)),
              [&closed] { return closed.load(std::memory_order_acquire); });
}

class NngRequester final : public RpcRequester {
 public:
  ~NngRequester() override { Close(); }

  TransferStatus Exchange(const std::string& endpoint, const std::string& request,
                          Deadline deadline, std::string* response) override {
    if (response == nullptr) return TransferStatus::kInvalidArgument;
    if (closed_.load(std::memory_order_acquire)) return TransferStatus::kClosed;

    // 发送阶段：EAGAIN 表示本次 send 未发生，可安全重试同一请求。
    for (;;) {
      if (closed_.load(std::memory_order_acquire)) return TransferStatus::kClosed;
      if (RemainingMs(deadline) <= 0) {
        MarkBroken();
        return TransferStatus::kTimeout;
      }
      int r = 0;
      {
        std::lock_guard<std::mutex> lock(io_mutex_);
        if (closed_.load(std::memory_order_acquire)) return TransferStatus::kClosed;
        TransferStatus ready = EnsureSocketLocked(endpoint);
        if (ready != TransferStatus::kOk) return ready;
        r = nng_send(sock_->Socket(), const_cast<char*>(request.data()),
                     request.size(), NNG_FLAG_NONBLOCK);
      }
      TransferStatus status = MapNngStatus(r);
      if (status == TransferStatus::kOk) break;
      if (status != TransferStatus::kWouldBlock) {
        MarkBroken();
        return status;
      }
      std::unique_lock<std::mutex> wait_lock(wait_mutex_);
      WaitSlice(cv_, wait_lock, deadline, closed_);
    }

    // 接收阶段：REQ 单在途由 Core 的单执行者保证；超时后标记连接失配，
    // 下次 Exchange 重建 socket，避免把迟到的响应配给后续请求。
    for (;;) {
      if (closed_.load(std::memory_order_acquire)) return TransferStatus::kClosed;
      if (RemainingMs(deadline) <= 0) {
        MarkBroken();
        return TransferStatus::kTimeout;
      }
      void* data = nullptr;
      size_t size = 0;
      int r = 0;
      {
        std::lock_guard<std::mutex> lock(io_mutex_);
        if (closed_.load(std::memory_order_acquire)) return TransferStatus::kClosed;
        r = nng_recv(sock_->Socket(), &data, &size,
                     NNG_FLAG_ALLOC | NNG_FLAG_NONBLOCK);
      }
      if (r == 0) {
        response->assign(static_cast<char*>(data), size);
        nng_free(data, size);
        return TransferStatus::kOk;
      }
      TransferStatus status = MapNngStatus(r);
      if (status == TransferStatus::kWouldBlock) {
        std::unique_lock<std::mutex> wait_lock(wait_mutex_);
        WaitSlice(cv_, wait_lock, deadline, closed_);
        continue;
      }
      MarkBroken();
      return status;
    }
  }

  void Close() override {
    if (closed_.exchange(true, std::memory_order_acq_rel)) return;
    {
      std::lock_guard<std::mutex> lock(io_mutex_);
      if (sock_) sock_->Close();
    }
    cv_.notify_all();
  }

 private:
  // 调用前必须持有 io_mutex_。
  TransferStatus EnsureSocketLocked(const std::string& endpoint) {
    if (sock_ && !broken_ && endpoint_ == endpoint) return TransferStatus::kOk;
    if (sock_) sock_->Close();
    sock_.reset();
    auto fresh = std::make_unique<NngSocket>();
    int r = nng_req0_open(&fresh->Socket());
    if (r != 0) return MapNngStatus(r);
    r = nng_dial(fresh->Socket(), endpoint.c_str(), nullptr, 0);
    if (r != 0) return MapNngStatus(r);
    sock_ = std::move(fresh);
    endpoint_ = endpoint;
    broken_ = false;
    return TransferStatus::kOk;
  }

  void MarkBroken() {
    std::lock_guard<std::mutex> lock(io_mutex_);
    broken_ = true;
    if (sock_) sock_->Close();  // REQ 状态机失配，下次重建
  }

  std::unique_ptr<NngSocket> sock_;
  std::string endpoint_;
  bool broken_ = false;  // 受 io_mutex_ 保护
  std::mutex io_mutex_;
  std::mutex wait_mutex_;
  std::condition_variable cv_;
  std::atomic<bool> closed_{false};
};

class NngResponder final : public RpcResponder {
 public:
  explicit NngResponder(std::unique_ptr<NngSocket> sock) : sock_(std::move(sock)) {}
  ~NngResponder() override { Close(); }

  TransferStatus Bind(std::string* endpoint) override {
    if (endpoint == nullptr) return TransferStatus::kInvalidArgument;
    if (closed_.load(std::memory_order_acquire)) return TransferStatus::kClosed;
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (closed_.load(std::memory_order_acquire)) return TransferStatus::kClosed;
    nng_listener listener = NNG_LISTENER_INITIALIZER;
    if (!ListenOnEphemeralPort(sock_->Socket(), &listener, endpoint)) {
      return TransferStatus::kIoError;
    }
    return TransferStatus::kOk;
  }

  TransferStatus Receive(std::string* request, std::unique_ptr<ReplyToken>* token,
                         Deadline deadline) override {
    if (request == nullptr || token == nullptr) {
      return TransferStatus::kInvalidArgument;
    }
    for (;;) {
      if (closed_.load(std::memory_order_acquire)) return TransferStatus::kClosed;
      if (RemainingMs(deadline) <= 0) return TransferStatus::kTimeout;
      void* data = nullptr;
      size_t size = 0;
      int r = 0;
      {
        std::lock_guard<std::mutex> lock(io_mutex_);
        if (closed_.load(std::memory_order_acquire)) return TransferStatus::kClosed;
        r = nng_recv(sock_->Socket(), &data, &size,
                     NNG_FLAG_ALLOC | NNG_FLAG_NONBLOCK);
      }
      if (r == 0) {
        request->assign(static_cast<char*>(data), size);
        nng_free(data, size);
        *token = std::make_unique<NngReplyToken>(this, next_token_++);
        return TransferStatus::kOk;
      }
      TransferStatus status = MapNngStatus(r);
      if (status == TransferStatus::kWouldBlock) {
        std::unique_lock<std::mutex> wait_lock(wait_mutex_);
        WaitSlice(cv_, wait_lock, deadline, closed_);
        continue;
      }
      return status;
    }
  }

  TransferStatus Reply(std::unique_ptr<ReplyToken> token,
                       const std::string& frame, Deadline deadline) override {
    auto* nng_token = dynamic_cast<NngReplyToken*>(token.get());
    if (nng_token == nullptr || nng_token->Owner() != this) {
      return TransferStatus::kInvalidArgument;
    }
    for (;;) {
      if (closed_.load(std::memory_order_acquire)) return TransferStatus::kClosed;
      if (RemainingMs(deadline) <= 0) return TransferStatus::kTimeout;
      int r = 0;
      {
        std::lock_guard<std::mutex> lock(io_mutex_);
        if (closed_.load(std::memory_order_acquire)) return TransferStatus::kClosed;
        r = nng_send(sock_->Socket(), const_cast<char*>(frame.data()),
                     frame.size(), NNG_FLAG_NONBLOCK);
      }
      TransferStatus status = MapNngStatus(r);
      if (status == TransferStatus::kOk) return status;
      if (status != TransferStatus::kWouldBlock) return status;
      std::unique_lock<std::mutex> wait_lock(wait_mutex_);
      WaitSlice(cv_, wait_lock, deadline, closed_);
    }
  }

  void Close() override {
    if (closed_.exchange(true, std::memory_order_acq_rel)) return;
    {
      std::lock_guard<std::mutex> lock(io_mutex_);
      if (sock_) sock_->Close();
    }
    cv_.notify_all();
  }

 private:
  std::unique_ptr<NngSocket> sock_;
  std::mutex io_mutex_;
  std::mutex wait_mutex_;
  std::condition_variable cv_;
  std::atomic<bool> closed_{false};
  std::uint64_t next_token_ = 1;  // 只由 Receive 所在 worker 访问
};

class NngRegistry final : public IRegistry {
 public:
  EndpointRecordId Register(const EndpointRecord& record) override {
    return discovery::Registry::instance().Register(record);
  }
  bool Unregister(EndpointRecordId id) override {
    return discovery::Registry::instance().Unregister(id);
  }
  std::vector<EndpointRecord> Query(
      RegistryKind kind, const std::string& name,
      const std::string& backend = std::string()) override {
    return discovery::Registry::instance().Query(kind, name, backend);
  }
  bool Has(RegistryKind kind, const std::string& name) override {
    return discovery::Registry::instance().Has(kind, name);
  }
  int Watch(RegistryKind kind, const std::string& name,
            WatchCallback&& cb) override {
    return discovery::Registry::instance().Watch(kind, name, std::move(cb));
  }
  void Unwatch(int id) override {
    discovery::Registry::instance().Unwatch(id);
  }
  void Clear() override { discovery::Registry::instance().Stop(); }
};

}  // namespace

namespace {

std::mutex g_assembly_mutex;
DiscoveryConfig g_discovery_config;
std::shared_ptr<InjectionContext> g_default_ctx;

std::string EnvValue(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr ? std::string(value) : std::string();
}

// 调用前持有 g_assembly_mutex。
void BootstrapLocked() {
  DiscoveryConfig config = g_discovery_config;
  const std::string rep = EnvValue("FIREFLY_DISCOVERY_REP_URL");
  const std::string pub = EnvValue("FIREFLY_DISCOVERY_PUB_URL");
  if (!rep.empty()) config.rep_url = rep;
  if (!pub.empty()) config.pub_url = pub;
  discovery::Registry::Configure(config.rep_url, config.pub_url);
  g_default_ctx = std::make_shared<InjectionContext>(
      InjectionContext{DefaultEndpointFactory(), DefaultRegistry()});
}

}  // namespace

class NngTransportFactory final : public IEndpointFactory {
 public:
  std::unique_ptr<Publisher> CreatePublisher() override {
    auto sock = std::make_unique<NngSocket>();
    if (nng_pub0_open(&sock->Socket()) != 0) return nullptr;
    return std::make_unique<NngPublisher>(std::move(sock));
  }
  std::unique_ptr<Subscriber> CreateSubscriber() override {
    auto sock = std::make_unique<NngSocket>();
    if (nng_sub0_open(&sock->Socket()) != 0) return nullptr;
    if (nng_setopt(sock->Socket(), NNG_OPT_SUB_SUBSCRIBE, "", size_t(0)) != 0) {
      return nullptr;
    }
    return std::make_unique<NngSubscriber>(std::move(sock));
  }
  std::unique_ptr<RpcRequester> CreateRequester() override {
    return std::make_unique<NngRequester>();
  }
  std::unique_ptr<RpcResponder> CreateResponder() override {
    auto sock = std::make_unique<NngSocket>();
    if (nng_rep0_open(&sock->Socket()) != 0) return nullptr;
    return std::make_unique<NngResponder>(std::move(sock));
  }
};

std::shared_ptr<IEndpointFactory> DefaultEndpointFactory() {
  static auto factory = std::make_shared<NngTransportFactory>();
  return factory;
}

std::shared_ptr<IRegistry> DefaultRegistry() {
  static auto registry = std::make_shared<NngRegistry>();
  return registry;
}

void ConfigureDiscovery(const DiscoveryConfig& config) {
  std::lock_guard<std::mutex> lock(g_assembly_mutex);
  g_discovery_config = config;
}

void BootstrapDefaultInjection() {
  std::lock_guard<std::mutex> lock(g_assembly_mutex);
  BootstrapLocked();
}

void ResetDefaultInjection() {
  std::shared_ptr<InjectionContext> old;
  {
    std::lock_guard<std::mutex> lock(g_assembly_mutex);
    old = std::move(g_default_ctx);
  }
  if (old && old->registry) old->registry->Clear();
}

std::shared_ptr<InjectionContext> DefaultInjection() {
  std::lock_guard<std::mutex> lock(g_assembly_mutex);
  if (!g_default_ctx) BootstrapLocked();
  return g_default_ctx;
}

std::shared_ptr<InjectionContext> ResolveInjection(
    const std::shared_ptr<InjectionContext>& ctx) {
  auto defaults = DefaultInjection();
  auto resolved = std::make_shared<InjectionContext>();
  resolved->endpoints =
      (ctx && ctx->endpoints) ? ctx->endpoints : defaults->endpoints;
  resolved->registry =
      (ctx && ctx->registry) ? ctx->registry : defaults->registry;
  return resolved;
}

}  // namespace detail
}  // namespace firefly
