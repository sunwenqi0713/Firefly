#ifndef FIREFLY_DETAIL_WRITER_IMPL_H_
#define FIREFLY_DETAIL_WRITER_IMPL_H_

#include <atomic>
#include <memory>
#include <string>

#include <nng/nng.h>
#include <nng/protocol/pubsub0/pub.h>

#include "firefly/common/log.h"
#include "firefly/detail/handles.h"
#include "firefly/detail/net.h"
#include "firefly/discovery.h"
#include "firefly/message/message_traits.h"
#include "firefly/node/channel_config.h"

namespace firefly {
namespace detail {

// PUB 发布者实现：持有一个 pub socket（绑定临时端口）。
template <typename MessageT>
struct WriterImpl : public WriterHandle {
  WriterImpl() = default;
  ~WriterImpl() override {
    if (registered_) {
      discovery::Registry::instance().Unregister(
          discovery::Kind::kWriter, config.channel_name, pub_url);
    }
    if (sock_.id != 0) {
      nng_close(sock_);
      sock_.id = 0;
    }
  }

  ChannelConfig config;
  std::string pub_url;                 // 本 writer 的发布地址
  nng_socket sock_{0};                 // nng_socket 为 {id} 值类型，默认 0 即可
  std::atomic<bool> registered_{false};
};

template <typename MessageT>
Result<std::shared_ptr<WriterHandle>> CreateWriter(const ChannelConfig& config) {
  static_assert(message::HasSerializer<MessageT>::value,
                "MessageT must derive from google::protobuf::Message.");

  const std::string where = "CreateWriter[channel='" + config.channel_name + "']: ";

  auto impl = std::make_shared<WriterImpl<MessageT>>();
  impl->config = config;

  int r = nng_pub0_open(&impl->sock_);
  if (r != 0) {
    FY_ERROR("writer[%s]: nng_pub0_open: %s", config.channel_name.c_str(),
             nng_strerror(r));
    return {nullptr, ErrorCode::kTransport,
            where + "nng_pub0_open failed: " + nng_strerror(r)};
  }

  nng_listener l{NNG_LISTENER_INITIALIZER};
  std::string url;
  if (!ListenOnEphemeralPort(impl->sock_, &l, &url)) {
    FY_ERROR("writer[%s]: bind ephemeral port failed",
             config.channel_name.c_str());
    nng_close(impl->sock_);
    impl->sock_.id = 0;
    return {nullptr, ErrorCode::kTransport,
            where + "bind ephemeral port failed (nng listener)"};
  }
  impl->pub_url = url;

  // 注册到注册中心：让 Reader 能发现并连接本 writer
  if (discovery::Registry::instance().Register(discovery::Kind::kWriter,
                                               config.channel_name, url)) {
    impl->registered_ = true;
  }
  FY_INFO("writer[%s] ok at %s", config.channel_name.c_str(), url.c_str());
  return {impl, ErrorCode::kOk, ""};
}

template <typename MessageT>
bool WriterHasReader(const WriterHandle* handle) {
  auto* impl = static_cast<const WriterImpl<MessageT>*>(handle);
  if (impl == nullptr) {
    return false;
  }
  return discovery::Registry::instance().Has(
      discovery::Kind::kReader, impl->config.channel_name);
}

// 写状态映射（供单测直接验证协议层错误→WriteStatus 的映射，不依赖真实拥塞）
inline WriteStatus MapNngSendError(int r) noexcept {
  if (r == 0) return WriteStatus::kAccepted;
  if (r == NNG_EAGAIN) return WriteStatus::kQueueFull;
  return WriteStatus::kTransportError;
}

template <typename MessageT>
WriteStatus WriterWrite(WriterHandle* handle,
                        const std::shared_ptr<const MessageT>& msg) {
  auto* impl = static_cast<WriterImpl<MessageT>*>(handle);
  if (impl == nullptr || impl->sock_.id == 0 || msg == nullptr) {
    return WriteStatus::kTransportError;
  }

  // nng PUB 即使无订阅者也会 send 成功；必须显式检查注册中心以获得 kNoSubscriber
  if (!WriterHasReader<MessageT>(handle)) {
    return WriteStatus::kNoSubscriber;
  }

  std::string bytes;
  if (!message::MessageTraits<MessageT>::Serialize(*msg, &bytes)) {
    FY_ERROR("writer[%s]: serialize failed", impl->config.channel_name.c_str());
    return WriteStatus::kSerializationError;
  }
  int r = nng_send(impl->sock_, const_cast<char*>(bytes.data()), bytes.size(), 0);
  if (r != 0) {
    FY_WARN("writer[%s]: nng_send: %s", impl->config.channel_name.c_str(),
            nng_strerror(r));
  }
  return MapNngSendError(r);
}

}  // namespace detail
}  // namespace firefly

#endif  // FIREFLY_DETAIL_WRITER_IMPL_H_