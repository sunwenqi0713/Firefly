#ifndef FIREFLY_DETAIL_RPC_FRAME_H_
#define FIREFLY_DETAIL_RPC_FRAME_H_

// ---------------------------------------------------------------------------
// RPC 响应帧（阶段一：让 ErrorCode 随响应跨进程传递）
// ---------------------------------------------------------------------------
// 帧格式（网络字节序无关，仅用于同构环境）：
//   [0..3]  magic  : "FFRP"
//   [4..7]  status : uint32_t，ErrorCode 的整数值
//   [8..N]  payload: Response 的 protobuf 序列化字节；0 字节合法（全默认消息）
//
// 为何需要 status 字段：旧实现用"空响应帧"表示失败，客户端无法区分
// 服务业务失败 / 超时 / 服务不存在 / 反序列化失败，全部折叠为 nullptr。
// 现在 status 字段承载 code，客户端可据以返回不同的 ErrorCode。
//
// 注意：本文件不引入任何 nng 头，公共头不得因包含本文件而泄漏传输层细节。
// ---------------------------------------------------------------------------

#include <cstdint>
#include <cstring>
#include <string>

#include "firefly/common/types.h"

namespace firefly {
namespace detail {

inline std::string EncodeResponse(ErrorCode code, const std::string& payload) {
  std::string frame("FFRP", 4);
  const std::uint32_t status = static_cast<std::uint32_t>(code);
  char status_bytes[sizeof(status)];
  std::memcpy(status_bytes, &status, sizeof(status));
  frame.append(status_bytes, sizeof(status));
  frame.append(payload);
  return frame;
}

// 返回 false 表示帧头非法（magic 不符 / 长度不足 / status 越界），
// 调用方应映射为 ErrorCode::kSerialization。
inline bool DecodeResponse(const std::string& frame, ErrorCode* code,
                           std::string* payload) {
  if (code == nullptr || payload == nullptr || frame.size() < 8) {
    return false;
  }
  if (std::memcmp(frame.data(), "FFRP", 4) != 0) {
    return false;
  }
  std::uint32_t status = 0;
  std::memcpy(&status, frame.data() + 4, sizeof(status));
  // 应答帧 status 的合法上界固定为 kTransport（ErrorCode 枚举序中的位置）。
  // kCancelled 是本地取消码、不进协议帧，故不随其加入而放宽上界。
  constexpr std::uint32_t kMaxWireStatus =
      static_cast<std::uint32_t>(ErrorCode::kTransport);
  if (status > kMaxWireStatus) {
    return false;
  }
  *code = static_cast<ErrorCode>(status);
  payload->assign(frame.data() + 8, frame.size() - 8);
  return true;
}

}  // namespace detail
}  // namespace firefly

#endif  // FIREFLY_DETAIL_RPC_FRAME_H_
