#ifndef FIREFLY_DETAIL_SERVICE_IMPL_H_
#define FIREFLY_DETAIL_SERVICE_IMPL_H_

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include <nng/nng.h>
#include <nng/protocol/reqrep0/rep.h>

#include "firefly/common/log.h"
#include "firefly/detail/handles.h"
#include "firefly/detail/net.h"
#include "firefly/detail/rpc_frame.h"
#include "firefly/discovery.h"
#include "firefly/message/message_traits.h"

namespace firefly {
namespace detail {

// REP 服务实现：绑定临时端口，一个工作线程循环接收请求并回响应。
template <typename Request, typename Response>
struct ServiceImpl : public ServiceHandle {
  using ServiceCallback =
      std::function<ServiceResult<Response>(const std::shared_ptr<const Request>&)>;

  std::string node_name;
  std::string service_name;
  ServiceCallback callback;

  nng_socket sock_{0};
  std::string url;
  bool registered_ = false;

  std::atomic<bool> running_{false};
  std::thread worker_;

  ~ServiceImpl() override { Shutdown(); }

  void Shutdown() {
    if (!running_.load() && sock_.id == 0) return;
    running_.store(false);
    if (worker_.joinable()) worker_.join();
    if (registered_) {
      discovery::Registry::instance().Unregister(
          discovery::Kind::kService, service_name, url);
      registered_ = false;
    }
    if (sock_.id != 0) {
      nng_close(sock_);
      sock_.id = 0;
    }
  }

  void Start() {
    running_.store(true);
    worker_ = std::thread([this]() { WorkerLoop(); });
  }

  void WorkerLoop() {
    nng_setopt_ms(sock_, NNG_OPT_RECVTIMEO, 200);
    while (running_.load()) {
      void* data = nullptr;
      size_t size = 0;
      int r = nng_recv(sock_, &data, &size, NNG_FLAG_ALLOC);
      if (r != 0) {
        if (r == NNG_ETIMEDOUT) continue;
        if (r == NNG_ECLOSED) break;
        FY_WARN("service[%s]: recv: %s", service_name.c_str(),
                nng_strerror(r));
        continue;
      }
      std::string req_bytes(static_cast<char*>(data), size);
      nng_free(data, size);

      auto req = std::make_shared<Request>();
      std::string resp_bytes;
      if (!message::MessageTraits<Request>::Deserialize(req_bytes, req.get())) {
        FY_WARN("service[%s]: bad request (%zu bytes)", service_name.c_str(),
                req_bytes.size());
        resp_bytes = EncodeResponse(ErrorCode::kSerialization, "");
      } else {
        ServiceResult<Response> result;
        try {
          result = callback(req);
        } catch (const std::exception& e) {
          FY_ERROR("service[%s]: callback: %s", service_name.c_str(), e.what());
          result.code = ErrorCode::kServiceFailed;
          result.response.reset();
        } catch (...) {
          FY_ERROR("service[%s]: callback threw", service_name.c_str());
          result.code = ErrorCode::kServiceFailed;
          result.response.reset();
        }

        std::string payload;
        if (result.code == ErrorCode::kOk && result.response) {
          if (!message::MessageTraits<Response>::Serialize(*result.response,
                                                          &payload)) {
            result.code = ErrorCode::kSerialization;
            result.response.reset();
            payload.clear();
          }
        } else if (result.code == ErrorCode::kOk && !result.response) {
          // 回调声称成功却没给 response：按业务失败处理，不返回空帧
          result.code = ErrorCode::kServiceFailed;
        }
        resp_bytes = EncodeResponse(result.code, payload);
      }
      nng_send(sock_, const_cast<char*>(resp_bytes.data()),
               resp_bytes.size(), 0);
    }
    FY_INFO("service[%s]: worker exit", service_name.c_str());
  }
};

// ---------- 接口层声明对应的操作函数 ----------

template <typename Request, typename Response>
Result<std::shared_ptr<ServiceHandle>> CreateService(
    const std::string& node_name, const std::string& service_name,
    const std::function<ServiceResult<Response>(
        const std::shared_ptr<const Request>&)>& callback) {
  static_assert(message::HasSerializer<Request>::value &&
                    message::HasSerializer<Response>::value,
                "Request/Response must derive from protobuf::Message.");

  const std::string where = "CreateService[service='" + service_name + "']: ";

  auto impl = std::make_shared<ServiceImpl<Request, Response>>();
  impl->node_name = node_name;
  impl->service_name = service_name;
  impl->callback = callback;

  int r = nng_rep0_open(&impl->sock_);
  if (r != 0) {
    FY_ERROR("service[%s]: nng_rep0_open: %s", service_name.c_str(),
             nng_strerror(r));
    return {nullptr, ErrorCode::kTransport,
            where + "nng_rep0_open failed: " + nng_strerror(r)};
  }

  nng_listener l{NNG_LISTENER_INITIALIZER};
  if (!ListenOnEphemeralPort(impl->sock_, &l, &impl->url)) {
    FY_ERROR("service[%s]: bind ephemeral port failed", service_name.c_str());
    nng_close(impl->sock_);
    impl->sock_.id = 0;
    return {nullptr, ErrorCode::kTransport,
            where + "bind ephemeral port failed (nng listener)"};
  }

  if (discovery::Registry::instance().Register(
          discovery::Kind::kService, service_name, impl->url)) {
    impl->registered_ = true;
  }
  impl->Start();
  FY_INFO("service[%s] ok at %s", service_name.c_str(), impl->url.c_str());
  return {impl, ErrorCode::kOk, ""};
}

template <typename Request, typename Response>
void ServiceShutdown(ServiceHandle* handle) {
  auto* impl = static_cast<ServiceImpl<Request, Response>*>(handle);
  if (impl) impl->Shutdown();
}

}  // namespace detail
}  // namespace firefly

#endif  // FIREFLY_DETAIL_SERVICE_IMPL_H_