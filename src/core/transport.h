#ifndef FIREFLY_CORE_TRANSPORT_H_
#define FIREFLY_CORE_TRANSPORT_H_

// ===========================================================================
// 数据传输角色契约（Core ↔ 后端适配器）
// ===========================================================================
// 四个 Core 只依赖本文件的 Publisher/Subscriber/RpcRequester/RpcResponder
// 及其内部工厂，不依赖 socket 类型、连接/重连细节或 NNG 错误码。
// 行为契约表见 include/firefly/detail/endpoint_api.h 顶部注释；本头只给
// 类型定义。公共模板不再包含本文件。
// ===========================================================================

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace firefly {
namespace detail {

// 角色接口的统一 I/O 状态：NNG 常量不进入 Core，所有后端错误在此收口。
enum class TransferStatus {
  kOk,
  kTimeout,
  kClosed,
  kWouldBlock,
  kIoError,
  kInvalidArgument,
};

using Deadline = std::chrono::steady_clock::time_point;

// 不透明应答句柄：Core 只回传，不解析内容（见 endpoint_api.h 契约表）。
class ReplyToken {
 public:
  virtual ~ReplyToken() = default;
};

class Publisher {
 public:
  virtual ~Publisher() = default;
  virtual TransferStatus Bind(std::string* endpoint) = 0;
  virtual TransferStatus Send(const std::string& bytes) = 0;
  virtual void Close() = 0;
};

class Subscriber {
 public:
  virtual ~Subscriber() = default;
  // 当前应连接的发布者地址全集；适配器自行去重、建连、断开已移除的地址。
  // 重复传入相同集合必须是 no-op，空串地址忽略。Close 后返回 kClosed。
  virtual TransferStatus UpdatePublishers(
      const std::vector<std::string>& endpoints) = 0;
  virtual TransferStatus Receive(std::string* bytes, Deadline deadline) = 0;
  virtual void Close() = 0;
};

class RpcRequester {
 public:
  virtual ~RpcRequester() = default;
  // deadline 为绝对时间点；连接/发送/接收各阻塞点前必须重算剩余预算。
  virtual TransferStatus Exchange(const std::string& endpoint,
                                  const std::string& request, Deadline deadline,
                                  std::string* response) = 0;
  virtual void Close() = 0;
};

class RpcResponder {
 public:
  virtual ~RpcResponder() = default;
  virtual TransferStatus Bind(std::string* endpoint) = 0;
  virtual TransferStatus Receive(std::string* request,
                                 std::unique_ptr<ReplyToken>* token,
                                 Deadline deadline) = 0;
  virtual TransferStatus Reply(std::unique_ptr<ReplyToken> token,
                               const std::string& frame, Deadline deadline) = 0;
  virtual void Close() = 0;
};

// 内部工厂：数据传输的唯一注入点；Registry 仍由 InjectionContext::registry。
class IEndpointFactory {
 public:
  virtual ~IEndpointFactory() = default;
  virtual std::unique_ptr<Publisher> CreatePublisher() = 0;
  virtual std::unique_ptr<Subscriber> CreateSubscriber() = 0;
  virtual std::unique_ptr<RpcRequester> CreateRequester() = 0;
  virtual std::unique_ptr<RpcResponder> CreateResponder() = 0;
};

}  // namespace detail
}  // namespace firefly

#endif  // FIREFLY_CORE_TRANSPORT_H_
