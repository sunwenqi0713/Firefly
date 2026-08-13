#ifndef FIREFLY_DETAIL_CLIENT_IMPL_H_
#define FIREFLY_DETAIL_CLIENT_IMPL_H_

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <thread>

#include <nng/nng.h>
#include <nng/protocol/reqrep0/req.h>

#include "firefly/common/log.h"
#include "firefly/detail/handles.h"
#include "firefly/discovery.h"
#include "firefly/message/message_traits.h"

namespace firefly {
namespace detail {

// REQ 客户端实现：REQ socket 与一个 REP 服务端点连接。
// 由于 REQ socket 同一时刻仅支持一个在途请求，所有操作经内部互斥串行化。
template <typename Request, typename Response>
struct ClientImpl : public ClientHandle {
  std::string node_name;
  std::string service_name;

  nng_socket sock_{0};
  mutable std::mutex io_mutex_;

  ~ClientImpl() override {
    if (sock_.id != 0) {
      nng_close(sock_);
      sock_.id = 0;
    }
  }

  // 确保与注册中心中该服务的某个端点建立连接
  bool EnsureConnected() {
    if (sock_.id != 0) return true;
    auto urls = discovery::Registry::instance().List(
        discovery::Kind::kService, service_name);
    if (urls.empty()) return false;
    int r = nng_req0_open(&sock_);
    if (r != 0) {
      FY_ERROR("client[%s]: nng_req0_open: %s", service_name.c_str(),
               nng_strerror(r));
      return false;
    }
    // 客户端主动拨号到第一个可用服务端点
    nng_dialer d{NNG_DIALER_INITIALIZER};
    r = nng_dial(sock_, urls.front().c_str(), &d, 0);
    if (r != 0) {
      FY_WARN("client[%s]: dial %s: %s", service_name.c_str(),
              urls.front().c_str(), nng_strerror(r));
      nng_close(sock_);
      sock_.id = 0;
      return false;
    }
    return true;
  }
};

// ---------- 接口层声明对应的操作函数 ----------

template <typename Request, typename Response>
std::shared_ptr<ClientHandle> CreateClient(const std::string& node_name,
                                           const std::string& service_name) {
  static_assert(message::HasSerializer<Request>::value &&
                    message::HasSerializer<Response>::value,
                "Request/Response must derive from protobuf::Message.");

  auto impl = std::make_shared<ClientImpl<Request, Response>>();
  impl->node_name = node_name;
  impl->service_name = service_name;
  return impl;
}

template <typename Request, typename Response>
void ClientShutdown(ClientHandle* handle) {
  auto* impl = static_cast<ClientImpl<Request, Response>*>(handle);
  if (!impl) return;
  if (impl->sock_.id != 0) {
    nng_close(impl->sock_);
    impl->sock_.id = 0;
  }
}

template <typename Request, typename Response>
std::shared_ptr<Response> ClientSendRequest(
    ClientHandle* handle, const std::shared_ptr<Request>& request,
    const std::chrono::milliseconds& timeout) {
  auto* impl = static_cast<ClientImpl<Request, Response>*>(handle);
  if (!impl || !request) return nullptr;
  std::lock_guard<std::mutex> lk(impl->io_mutex_);

  if (!impl->EnsureConnected()) {
    FY_WARN("client[%s]: no service endpoint", impl->service_name.c_str());
    return nullptr;
  }

  std::string bytes;
  if (!message::MessageTraits<Request>::Serialize(*request, &bytes)) {
    return nullptr;
  }
  nng_setopt_ms(impl->sock_, NNG_OPT_SENDTIMEO,
                static_cast<int>(timeout.count()));
  nng_setopt_ms(impl->sock_, NNG_OPT_RECVTIMEO,
                static_cast<int>(timeout.count()));

  int r = nng_send(impl->sock_, const_cast<char*>(bytes.data()), bytes.size(), 0);
  if (r != 0) {
    FY_WARN("client[%s]: send: %s", impl->service_name.c_str(),
            nng_strerror(r));
    return nullptr;
  }
  void* data = nullptr;
  size_t size = 0;
  r = nng_recv(impl->sock_, &data, &size, NNG_FLAG_ALLOC);
  if (r != 0) {
    FY_WARN("client[%s]: recv: %s", impl->service_name.c_str(),
            nng_strerror(r));
    // REQ 协议在超时后 socket 状态可能不一致，关闭以重置
    if (r == NNG_ETIMEDOUT) {
      nng_close(impl->sock_);
      impl->sock_.id = 0;
    }
    return nullptr;
  }
  std::string resp_bytes(static_cast<char*>(data), size);
  nng_free(data, size);

  if (resp_bytes.empty()) {
    return nullptr;  // 服务端无响应（失败）
  }
  auto resp = std::make_shared<Response>();
  if (!message::MessageTraits<Response>::Deserialize(resp_bytes, resp.get())) {
    return nullptr;
  }
  return resp;
}

template <typename Request, typename Response>
std::shared_future<std::shared_ptr<Response>> ClientAsyncSendRequest(
    ClientHandle* handle, const std::shared_ptr<Request>& request) {
  using SharedResponse = std::shared_ptr<Response>;
  auto promise = std::make_shared<std::promise<SharedResponse>>();
  std::shared_future<SharedResponse> future = promise->get_future();

  std::thread([handle, request, promise]() mutable {
    auto resp = ClientSendRequest<Request, Response>(
        handle, request, std::chrono::milliseconds(5000));
    promise->set_value(resp);
  }).detach();

  return future;
}

template <typename Request, typename Response>
std::shared_future<std::shared_ptr<Response>>
ClientAsyncSendRequestWithCallback(
    ClientHandle* handle, const std::shared_ptr<Request>& request,
    std::function<void(std::shared_future<std::shared_ptr<Response>>&)> cb) {
  using SharedResponse = std::shared_ptr<Response>;
  auto promise = std::make_shared<std::promise<SharedResponse>>();
  std::shared_future<SharedResponse> future = promise->get_future();

  std::thread([promise, future, cb, handle, request]() mutable {
    auto resp = ClientSendRequest<Request, Response>(
        handle, request, std::chrono::milliseconds(5000));
    promise->set_value(resp);
    if (cb) {
      cb(future);
    }
  }).detach();

  return future;
}

template <typename Request, typename Response>
bool ClientServiceIsReady(const ClientHandle* handle) {
  auto* impl = static_cast<const ClientImpl<Request, Response>*>(handle);
  if (!impl) return false;
  return discovery::Registry::instance().Has(
      discovery::Kind::kService, impl->service_name);
}

template <typename Request, typename Response>
bool ClientWaitForService(const ClientHandle* handle,
                          std::chrono::nanoseconds timeout) {
  auto* impl = static_cast<const ClientImpl<Request, Response>*>(handle);
  if (!impl) return false;

  // timeout < 0 表示无限等待；timeout == 0 仅检查当前状态。
  const auto infinite = timeout.count() < 0;
  const auto deadline =
      infinite ? std::chrono::steady_clock::time_point::max()
               : std::chrono::steady_clock::now() + timeout;

  constexpr auto poll = std::chrono::milliseconds(50);
  while (true) {
    if (discovery::Registry::instance().Has(discovery::Kind::kService,
                                            impl->service_name)) {
      return true;
    }
    if (!infinite && std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(poll);
  }
}

}  // namespace detail
}  // namespace firefly

#endif  // FIREFLY_DETAIL_CLIENT_IMPL_H_