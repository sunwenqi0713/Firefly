#ifndef FIREFLY_DETAIL_NET_H_
#define FIREFLY_DETAIL_NET_H_

#include <cstdint>
#include <random>
#include <string>

#include <nng/nng.h>
#include <nng/protocol/pubsub0/pub.h>
#include <nng/protocol/pubsub0/sub.h>
#include <nng/protocol/reqrep0/rep.h>
#include <nng/protocol/reqrep0/req.h>

namespace firefly {
namespace detail {

// 绑定一个 TCP 监听器到临时端口，返回实际地址（tcp://127.0.0.1:PORT）
// 失败返回空串。监听器句柄写回 *lp（由调用方持有避免过早关闭）。
inline bool ListenOnEphemeralPort(nng_socket sock, nng_listener* lp,
                                  std::string* out_url) {
  for (int attempt = 0; attempt < 20; ++attempt) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint16_t> dist(20000, 60000);
    int port = dist(gen);
    std::string url = "tcp://127.0.0.1:" + std::to_string(port);
    nng_listener l = NNG_LISTENER_INITIALIZER;
    int r = nng_listen(sock, url.c_str(), &l, 0);
    if (r == 0) {
      *lp = l;
      *out_url = url;
      return true;
    }
    if (r != NNG_EADDRINUSE) {
      return false;  // 非端口占用错误，直接失败
    }
  }
  return false;
}

// 从注册中心查询的字符串地址构建 tcp URL（保证 tcp:// 前缀）
inline std::string ToTcpUrl(const std::string& host_port) {
  if (host_port.rfind("tcp://", 0) == 0) {
    return host_port;
  }
  return "tcp://" + host_port;
}

}  // namespace detail
}  // namespace firefly

#endif  // FIREFLY_DETAIL_NET_H_