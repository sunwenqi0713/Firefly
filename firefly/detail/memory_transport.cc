#include "firefly/detail/backend_access.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace firefly {
namespace detail {
namespace {

using Deadline = std::chrono::steady_clock::time_point;

struct MemorySubState {
  std::mutex mutex;
  std::condition_variable cv;
  std::deque<std::string> frames;
  bool closed = false;
};

struct MemoryPubChannel {
  std::mutex mutex;
  std::map<std::uint64_t, std::weak_ptr<MemorySubState>> subscribers;
  bool closed = false;
};

struct MemoryResponse {
  std::mutex mutex;
  std::condition_variable cv;
  bool ready = false;
  bool closed = false;
  std::string frame;
};

struct MemoryRpcRequest {
  std::string bytes;
  std::shared_ptr<MemoryResponse> response;
};

struct MemoryRpcEndpoint {
  std::mutex mutex;
  std::condition_variable cv;
  std::deque<MemoryRpcRequest> requests;
  bool closed = false;
};

struct MemoryBus {
  std::mutex mutex;
  std::map<std::string, std::weak_ptr<MemoryPubChannel>> pub_channels;
  std::map<std::string, std::weak_ptr<MemoryRpcEndpoint>> rpc_endpoints;
  std::atomic<std::uint64_t> next_id{1};

  std::string NextId(const char* prefix) {
    return std::string(prefix) + std::to_string(next_id.fetch_add(1));
  }
};

class MemoryPublisher final : public Publisher {
 public:
  explicit MemoryPublisher(std::shared_ptr<MemoryBus> bus) : bus_(std::move(bus)) {}

  TransferStatus Bind(std::string* endpoint) override {
    if (endpoint == nullptr) return TransferStatus::kInvalidArgument;
    if (closed_.load(std::memory_order_acquire)) return TransferStatus::kClosed;
    auto channel = std::make_shared<MemoryPubChannel>();
    std::lock_guard<std::mutex> lock(bus_->mutex);
    if (closed_.load(std::memory_order_acquire)) return TransferStatus::kClosed;
    const std::string id = bus_->NextId("mem://pub/");
    bus_->pub_channels[id] = channel;
    channel_ = channel;
    *endpoint = id;
    return TransferStatus::kOk;
  }

  TransferStatus Send(const std::string& bytes) override {
    if (closed_.load(std::memory_order_acquire)) return TransferStatus::kClosed;
    auto channel = channel_;
    if (!channel) return TransferStatus::kIoError;
    std::lock_guard<std::mutex> lock(channel->mutex);
    if (channel->closed) return TransferStatus::kClosed;
    for (auto it = channel->subscribers.begin(); it != channel->subscribers.end();) {
      auto sub = it->second.lock();
      if (!sub) {
        it = channel->subscribers.erase(it);
        continue;
      }
      {
        std::lock_guard<std::mutex> sub_lock(sub->mutex);
        if (!sub->closed) sub->frames.push_back(bytes);
      }
      sub->cv.notify_one();
      ++it;
    }
    return TransferStatus::kOk;
  }

  void Close() override {
    if (closed_.exchange(true, std::memory_order_acq_rel)) return;
    auto channel = channel_;
    if (!channel) return;
    std::vector<std::shared_ptr<MemorySubState>> subs;
    {
      std::lock_guard<std::mutex> lock(channel->mutex);
      channel->closed = true;
      for (const auto& kv : channel->subscribers) {
        if (auto sub = kv.second.lock()) subs.push_back(std::move(sub));
      }
    }
    for (const auto& sub : subs) {
      std::lock_guard<std::mutex> lock(sub->mutex);
      sub->cv.notify_all();
    }
  }

 private:
  std::shared_ptr<MemoryBus> bus_;
  std::shared_ptr<MemoryPubChannel> channel_;
  std::atomic<bool> closed_{false};
};

class MemorySubscriber final : public Subscriber {
 public:
  explicit MemorySubscriber(std::shared_ptr<MemoryBus> bus)
      : bus_(std::move(bus)), state_(std::make_shared<MemorySubState>()),
        subscriber_id_(bus_->next_id.fetch_add(1)) {}

  TransferStatus UpdatePublishers(
      const std::vector<std::string>& endpoints) override {
    if (closed_.load(std::memory_order_acquire)) return TransferStatus::kClosed;
    std::set<std::string> desired;
    for (const auto& endpoint : endpoints) {
      if (!endpoint.empty()) desired.insert(endpoint);
    }
    std::lock_guard<std::mutex> lock(bus_->mutex);
    if (closed_.load(std::memory_order_acquire)) return TransferStatus::kClosed;
    for (const auto& endpoint : connected_) {
      if (desired.count(endpoint) != 0) continue;
      auto it = bus_->pub_channels.find(endpoint);
      if (it == bus_->pub_channels.end()) continue;
      auto channel = it->second.lock();
      if (!channel) continue;
      std::lock_guard<std::mutex> channel_lock(channel->mutex);
      channel->subscribers.erase(subscriber_id_);
    }
    for (const auto& endpoint : desired) {
      if (connected_.count(endpoint) != 0) continue;
      auto it = bus_->pub_channels.find(endpoint);
      if (it == bus_->pub_channels.end()) continue;  // 尚未 Bind，后续 Watch 再补
      auto channel = it->second.lock();
      if (!channel) continue;
      std::lock_guard<std::mutex> channel_lock(channel->mutex);
      if (!channel->closed) channel->subscribers[subscriber_id_] = state_;
    }
    connected_ = std::move(desired);
    return TransferStatus::kOk;
  }

  TransferStatus Receive(std::string* bytes, Deadline deadline) override {
    if (bytes == nullptr) return TransferStatus::kInvalidArgument;
    std::unique_lock<std::mutex> lock(state_->mutex);
    for (;;) {
      if (closed_.load(std::memory_order_acquire) || state_->closed) {
        return TransferStatus::kClosed;
      }
      if (!state_->frames.empty()) {
        *bytes = std::move(state_->frames.front());
        state_->frames.pop_front();
        return TransferStatus::kOk;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        return TransferStatus::kTimeout;
      }
      state_->cv.wait_until(lock, deadline, [this] {
        return closed_.load(std::memory_order_acquire) || state_->closed ||
               !state_->frames.empty();
      });
    }
  }

  void Close() override {
    if (closed_.exchange(true, std::memory_order_acq_rel)) return;
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->closed = true;
    state_->cv.notify_all();
  }

 private:
  std::shared_ptr<MemoryBus> bus_;
  std::shared_ptr<MemorySubState> state_;
  std::uint64_t subscriber_id_ = 0;
  std::set<std::string> connected_;  // 受 bus_->mutex 保护
  std::atomic<bool> closed_{false};
};

class MemoryReplyToken final : public ReplyToken {
 public:
  MemoryReplyToken(std::shared_ptr<MemoryRpcEndpoint> owner,
                   std::shared_ptr<MemoryRpcRequest> request)
      : owner_(std::move(owner)), request_(std::move(request)) {}

  const std::shared_ptr<MemoryRpcEndpoint>& Owner() const noexcept {
    return owner_;
  }
  const std::shared_ptr<MemoryResponse>& Response() const noexcept {
    return request_->response;
  }

 private:
  std::shared_ptr<MemoryRpcEndpoint> owner_;
  std::shared_ptr<MemoryRpcRequest> request_;
};

class MemoryRequester final : public RpcRequester {
 public:
  explicit MemoryRequester(std::shared_ptr<MemoryBus> bus) : bus_(std::move(bus)) {}
  ~MemoryRequester() override { Close(); }

  TransferStatus Exchange(const std::string& endpoint, const std::string& request,
                          Deadline deadline, std::string* response) override {
    if (response == nullptr) return TransferStatus::kInvalidArgument;
    if (closed_.load(std::memory_order_acquire)) return TransferStatus::kClosed;
    if (std::chrono::steady_clock::now() >= deadline) return TransferStatus::kTimeout;

    std::shared_ptr<MemoryRpcEndpoint> target;
    {
      std::lock_guard<std::mutex> lock(bus_->mutex);
      auto it = bus_->rpc_endpoints.find(endpoint);
      if (it == bus_->rpc_endpoints.end()) return TransferStatus::kIoError;
      target = it->second.lock();
    }
    if (!target) return TransferStatus::kIoError;

    auto box = std::make_shared<MemoryResponse>();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (closed_.load(std::memory_order_acquire)) return TransferStatus::kClosed;
      active_.insert(box);
    }
    {
      std::lock_guard<std::mutex> lock(target->mutex);
      if (target->closed) {
        std::lock_guard<std::mutex> own(mutex_);
        active_.erase(box);
        return TransferStatus::kClosed;
      }
      target->requests.push_back(MemoryRpcRequest{request, box});
      target->cv.notify_one();
    }

    bool ready = false;
    bool cancelled = false;
    std::string frame;
    {
      std::unique_lock<std::mutex> lock(box->mutex);
      box->cv.wait_until(lock, deadline, [&] {
        return box->ready || box->closed || closed_.load(std::memory_order_acquire);
      });
      ready = box->ready;
      cancelled = box->closed || closed_.load(std::memory_order_acquire);
      if (ready) frame = box->frame;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      active_.erase(box);
    }
    if (cancelled) return TransferStatus::kClosed;
    if (!ready) return TransferStatus::kTimeout;
    *response = std::move(frame);
    return TransferStatus::kOk;
  }

  void Close() override {
    if (closed_.exchange(true, std::memory_order_acq_rel)) return;
    std::vector<std::shared_ptr<MemoryResponse>> boxes;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      boxes.assign(active_.begin(), active_.end());
    }
    for (const auto& box : boxes) {
      std::lock_guard<std::mutex> lock(box->mutex);
      box->closed = true;
      box->cv.notify_all();
    }
  }

 private:
  std::shared_ptr<MemoryBus> bus_;
  std::mutex mutex_;
  std::set<std::shared_ptr<MemoryResponse>> active_;
  std::atomic<bool> closed_{false};
};

class MemoryResponder final : public RpcResponder {
 public:
  explicit MemoryResponder(std::shared_ptr<MemoryBus> bus) : bus_(std::move(bus)) {}
  ~MemoryResponder() override { Close(); }

  TransferStatus Bind(std::string* endpoint) override {
    if (endpoint == nullptr) return TransferStatus::kInvalidArgument;
    if (closed_.load(std::memory_order_acquire)) return TransferStatus::kClosed;
    auto target = std::make_shared<MemoryRpcEndpoint>();
    std::lock_guard<std::mutex> lock(bus_->mutex);
    if (closed_.load(std::memory_order_acquire)) return TransferStatus::kClosed;
    const std::string id = bus_->NextId("mem://rpc/");
    bus_->rpc_endpoints[id] = target;
    endpoint_ = id;
    target_ = target;
    *endpoint = id;
    return TransferStatus::kOk;
  }

  TransferStatus Receive(std::string* request,
                         std::unique_ptr<ReplyToken>* token,
                         Deadline deadline) override {
    if (request == nullptr || token == nullptr) {
      return TransferStatus::kInvalidArgument;
    }
    auto target = target_;
    if (!target) return TransferStatus::kIoError;
    std::unique_lock<std::mutex> lock(target->mutex);
    for (;;) {
      if (closed_.load(std::memory_order_acquire) || target->closed) {
        return TransferStatus::kClosed;
      }
      if (!target->requests.empty()) {
        auto pending = std::move(target->requests.front());
        target->requests.pop_front();
        *request = pending.bytes;
        *token = std::make_unique<MemoryReplyToken>(
            target, std::make_shared<MemoryRpcRequest>(std::move(pending)));
        return TransferStatus::kOk;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        return TransferStatus::kTimeout;
      }
      target->cv.wait_until(lock, deadline, [&] {
        return closed_.load(std::memory_order_acquire) || target->closed ||
               !target->requests.empty();
      });
    }
  }

  TransferStatus Reply(std::unique_ptr<ReplyToken> token,
                       const std::string& frame, Deadline deadline) override {
    (void)deadline;
    auto* memory_token = dynamic_cast<MemoryReplyToken*>(token.get());
    if (memory_token == nullptr || memory_token->Owner() != target_) {
      return TransferStatus::kInvalidArgument;
    }
    const auto& box = memory_token->Response();
    std::lock_guard<std::mutex> lock(box->mutex);
    if (!box->closed) {
      box->frame = frame;
      box->ready = true;
    }
    box->cv.notify_all();
    return TransferStatus::kOk;
  }

  void Close() override {
    if (closed_.exchange(true, std::memory_order_acq_rel)) return;
    auto target = target_;
    if (target) {
      {
        std::lock_guard<std::mutex> lock(target->mutex);
        target->closed = true;
      }
      target->cv.notify_all();
    }
    std::lock_guard<std::mutex> lock(bus_->mutex);
    auto it = bus_->rpc_endpoints.find(endpoint_);
    if (it != bus_->rpc_endpoints.end() && it->second.lock() == target) {
      bus_->rpc_endpoints.erase(it);
    }
  }

 private:
  std::shared_ptr<MemoryBus> bus_;
  std::shared_ptr<MemoryRpcEndpoint> target_;
  std::string endpoint_;
  std::atomic<bool> closed_{false};
};

class MemoryEndpointFactory final : public IEndpointFactory {
 public:
  MemoryEndpointFactory() : bus_(std::make_shared<MemoryBus>()) {}

  std::unique_ptr<Publisher> CreatePublisher() override {
    return std::make_unique<MemoryPublisher>(bus_);
  }
  std::unique_ptr<Subscriber> CreateSubscriber() override {
    return std::make_unique<MemorySubscriber>(bus_);
  }
  std::unique_ptr<RpcRequester> CreateRequester() override {
    return std::make_unique<MemoryRequester>(bus_);
  }
  std::unique_ptr<RpcResponder> CreateResponder() override {
    return std::make_unique<MemoryResponder>(bus_);
  }

 private:
  std::shared_ptr<MemoryBus> bus_;
};

}  // namespace

std::shared_ptr<IEndpointFactory> MakeMemoryEndpointFactory() {
  return std::make_shared<MemoryEndpointFactory>();
}

}  // namespace detail
}  // namespace firefly
