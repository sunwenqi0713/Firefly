// creation_failure_test.cpp：端点创建失败路径不得泄漏 Core。
// Core 曾在 self_keepalive_ 建立之后才做工厂/绑定/注册/Watch；任一失败都会
// 留下自引用环，core（及其持有的业务回调捕获对象）永远不析构。
// 这里通过注入失败工厂/Registry 覆盖 Reader/Service 的每条失败路径，并用
// 回调捕获对象的 weak_ptr 判定 core 是否真的被回收。
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "firefly/detail/endpoint_api.h"
#include "firefly/detail/interfaces.h"

using namespace firefly::detail;
using firefly::CallbackResult;
using firefly::ErrorCode;
using firefly::MessageContext;
using firefly::ReaderConfig;

namespace {

enum class FailAt {
  kNone,
  kCreateSubscriber,
  kCreateResponder,
  kBind,
  kRegister,
  kWatch,
};

class StubPublisher final : public Publisher {
 public:
  TransferStatus Bind(std::string* endpoint) override {
    if (endpoint) *endpoint = "stub://pub";
    return TransferStatus::kOk;
  }
  TransferStatus Send(const std::string&) override { return TransferStatus::kOk; }
  void Close() override {}
};

class StubSubscriber final : public Subscriber {
 public:
  TransferStatus UpdatePublishers(
      const std::vector<std::string>&) override {
    return TransferStatus::kOk;
  }
  TransferStatus Receive(std::string*, Deadline) override {
    return TransferStatus::kClosed;
  }
  void Close() override {}
};

class StubRequester final : public RpcRequester {
 public:
  TransferStatus Exchange(const std::string&, const std::string&, Deadline,
                          std::string*) override {
    return TransferStatus::kClosed;
  }
  void Close() override {}
};

class StubResponder final : public RpcResponder {
 public:
  explicit StubResponder(bool fail_bind) : fail_bind_(fail_bind) {}
  TransferStatus Bind(std::string* endpoint) override {
    if (fail_bind_) return TransferStatus::kIoError;
    if (endpoint) *endpoint = "stub://rep";
    return TransferStatus::kOk;
  }
  TransferStatus Receive(std::string*, std::unique_ptr<ReplyToken>*,
                         Deadline) override {
    return TransferStatus::kClosed;
  }
  TransferStatus Reply(std::unique_ptr<ReplyToken>, const std::string&,
                       Deadline) override {
    return TransferStatus::kOk;
  }
  void Close() override {}

 private:
  bool fail_bind_;
};

class FailingFactory final : public IEndpointFactory {
 public:
  explicit FailingFactory(FailAt at) : at_(at) {}
  std::unique_ptr<Publisher> CreatePublisher() override {
    return std::make_unique<StubPublisher>();
  }
  std::unique_ptr<Subscriber> CreateSubscriber() override {
    if (at_ == FailAt::kCreateSubscriber) return nullptr;
    return std::make_unique<StubSubscriber>();
  }
  std::unique_ptr<RpcRequester> CreateRequester() override {
    return std::make_unique<StubRequester>();
  }
  std::unique_ptr<RpcResponder> CreateResponder() override {
    if (at_ == FailAt::kCreateResponder) return nullptr;
    return std::make_unique<StubResponder>(at_ == FailAt::kBind);
  }

 private:
  FailAt at_;
};

class FailingRegistry final : public IRegistry {
 public:
  explicit FailingRegistry(FailAt at) : at_(at) {}

  EndpointRecordId Register(const EndpointRecord& rec) override {
    if (at_ == FailAt::kRegister) return 0;
    const auto id = next_id_++;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      table_[id] = rec;
      table_[id].id = id;
    }
    return id;
  }
  bool Unregister(EndpointRecordId id) override {
    std::lock_guard<std::mutex> lock(mutex_);
    return table_.erase(id) != 0;
  }
  std::vector<EndpointRecord> Query(RegistryKind, const std::string&,
                                    const std::string&) override {
    return {};
  }
  bool Has(RegistryKind, const std::string&) override { return false; }
  int Watch(RegistryKind, const std::string&, WatchCallback&&) override {
    return at_ == FailAt::kWatch ? 0 : next_watch_++;
  }
  void Unwatch(int) override {}
  void Clear() override {}

 private:
  FailAt at_;
  EndpointRecordId next_id_ = 1;
  int next_watch_ = 1;
  std::mutex mutex_;
  std::map<EndpointRecordId, EndpointRecord> table_;
};

int CheckReader(FailAt at) {
  auto payload = std::make_shared<int>(7);
  std::weak_ptr<int> weak = payload;
  auto ctx = std::make_shared<InjectionContext>();
  ctx->endpoints = std::make_shared<FailingFactory>(at);
  ctx->registry = std::make_shared<FailingRegistry>(at);
  ReaderConfig config;
  config.channel_name = "creation_failure_reader";
  auto r = CreateReaderCore(
      config, [payload](const MessageContext&, const std::string&) {
        return CallbackResult::kOk;
      }, ctx);
  if (r) return 1;
  payload.reset();
  // core 若泄漏，其 on_bytes 仍持有 payload。
  return weak.expired() ? 0 : 2;
}

int CheckService(FailAt at) {
  auto payload = std::make_shared<int>(7);
  std::weak_ptr<int> weak = payload;
  auto ctx = std::make_shared<InjectionContext>();
  ctx->endpoints = std::make_shared<FailingFactory>(at);
  ctx->registry = std::make_shared<FailingRegistry>(at);
  auto r = CreateServiceCore(
      "creation_failure_node", "creation_failure_service",
      [payload](const std::string&, ErrorCode*) { return std::string(); }, ctx);
  if (r) return 3;
  payload.reset();
  return weak.expired() ? 0 : 4;
}

}  // namespace

int main() {
  if (int e = CheckReader(FailAt::kCreateSubscriber); e != 0) return e;
  if (int e = CheckReader(FailAt::kRegister); e != 0) return e;
  if (int e = CheckReader(FailAt::kWatch); e != 0) return e;
  if (int e = CheckService(FailAt::kCreateResponder); e != 0) return e;
  if (int e = CheckService(FailAt::kBind); e != 0) return e;
  if (int e = CheckService(FailAt::kRegister); e != 0) return e;
  return 0;
}
