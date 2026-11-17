#include "firefly/detail/interfaces.h"

#include <cstdlib>
#include <memory>
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

class NngSocket final : public ISocket {
 public:
  explicit NngSocket(TransportKind kind) : kind_(kind) {}

  ~NngSocket() override { Close(); }

  TransportKind Kind() const override { return kind_; }

  int BindEphemeral(std::string* url) override {
    if (sock_.id == 0 || url == nullptr) return NNG_EINVAL;
    nng_listener listener = NNG_LISTENER_INITIALIZER;
    if (!ListenOnEphemeralPort(sock_, &listener, url)) {
      return NNG_EADDRINUSE;
    }
    return 0;
  }

  int Dial(const std::string& url) override {
    if (sock_.id == 0) return NNG_ECLOSED;
    return nng_dial(sock_, url.c_str(), nullptr, 0);
  }

  int Send(const std::string& bytes) override {
    if (sock_.id == 0) return NNG_ECLOSED;
    return nng_send(sock_, const_cast<char*>(bytes.data()), bytes.size(), 0);
  }

  int Recv(std::string* bytes, int timeout_ms) override {
    if (sock_.id == 0 || bytes == nullptr) return NNG_ECLOSED;
    nng_setopt_ms(sock_, NNG_OPT_RECVTIMEO, timeout_ms);
    void* data = nullptr;
    size_t size = 0;
    int r = nng_recv(sock_, &data, &size, NNG_FLAG_ALLOC);
    if (r != 0) {
      if (r == NNG_ETIMEDOUT) return kIoTimeout;
      if (r == NNG_ECLOSED) return kIoClosed;
      return r;
    }
    bytes->assign(static_cast<char*>(data), size);
    nng_free(data, size);
    return kIoOk;
  }

  int SetRecvTimeout(int ms) override {
    if (sock_.id == 0) return NNG_ECLOSED;
    return nng_setopt_ms(sock_, NNG_OPT_RECVTIMEO, ms);
  }

  int SetSendTimeout(int ms) override {
    if (sock_.id == 0) return NNG_ECLOSED;
    return nng_setopt_ms(sock_, NNG_OPT_SENDTIMEO, ms);
  }

  bool Valid() const override { return sock_.id != 0; }

  void Close() override {
    if (sock_.id != 0) {
      nng_close(sock_);
      sock_.id = 0;
    }
  }

  nng_socket& Socket() noexcept { return sock_; }

 private:
  TransportKind kind_;
  nng_socket sock_{0};
};

class NngTransportFactory final : public ITransportFactory {
 public:
  std::unique_ptr<ISocket> Create(TransportKind kind) override {
    auto sock = std::make_unique<NngSocket>(kind);
    int r = 0;
    switch (kind) {
      case TransportKind::kPub:
        r = nng_pub0_open(&sock->Socket());
        break;
      case TransportKind::kSub:
        r = nng_sub0_open(&sock->Socket());
        if (r == 0) {
          r = nng_setopt(sock->Socket(), NNG_OPT_SUB_SUBSCRIBE, "", size_t(0));
        }
        break;
      case TransportKind::kReq:
        r = nng_req0_open(&sock->Socket());
        break;
      case TransportKind::kRep:
        r = nng_rep0_open(&sock->Socket());
        break;
    }
    if (r != 0) {
      FY_ERROR("transport: open %d failed: %s", static_cast<int>(kind),
               nng_strerror(r));
      return nullptr;
    }
    return sock;
  }
};

class DefaultRegistryAdapter final : public IRegistry {
 public:
  bool Register(RegistryKind kind, const std::string& name,
                const std::string& url) override {
    return discovery::Registry::instance().Register(kind, name, url);
  }
  bool Unregister(RegistryKind kind, const std::string& name,
                  const std::string& url) override {
    return discovery::Registry::instance().Unregister(kind, name, url);
  }
  std::vector<std::string> List(RegistryKind kind,
                                const std::string& name) override {
    return discovery::Registry::instance().List(kind, name);
  }
  bool Has(RegistryKind kind, const std::string& name) override {
    return discovery::Registry::instance().Has(kind, name);
  }
  int Subscribe(RegistryKind kind, const std::string& name,
                std::function<void(const RegistryEvent&)>&& cb) override {
    return discovery::Registry::instance().Subscribe(kind, name, std::move(cb));
  }
  void Unsubscribe(int id) override {
    discovery::Registry::instance().Unsubscribe(id);
  }
};

}  // namespace

std::shared_ptr<ITransportFactory> DefaultTransportFactory() {
  static auto factory = std::make_shared<NngTransportFactory>();
  return factory;
}

std::shared_ptr<IRegistry> DefaultRegistry() {
  static auto registry = std::make_shared<DefaultRegistryAdapter>();
  return registry;
}

std::shared_ptr<InjectionContext> DefaultInjection() {
  static auto ctx = std::make_shared<InjectionContext>(
      InjectionContext{DefaultTransportFactory(), DefaultRegistry()});
  return ctx;
}

}  // namespace detail
}  // namespace firefly
