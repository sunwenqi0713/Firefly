#ifndef FIREFLY_DETAIL_ENDPOINT_API_H_
#define FIREFLY_DETAIL_ENDPOINT_API_H_

// ===========================================================================
// 端点的非模板传输 API（detail 层，阶段三任务 2）
// ===========================================================================
// 本头文件不含 nng、不含任何 *_impl.h。模板端点（Writer/Reader/Service/
// Client）只做 protobuf 序列化 shim，所有 socket/dial/线程/统计逻辑都在
// 对应的 *_core.cc 中实现。用户自定义 protobuf 消息无需显式实例化。
// 端点方法签名与阶段一/二的语义（WriteStatus/ErrorCode/Cancel）保持不变。
// ===========================================================================

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "firefly/common/types.h"
#include "firefly/detail/handles.h"
#include "firefly/detail/interfaces.h"
#include "firefly/node/channel_config.h"

namespace firefly {
namespace detail {

// ---------------- Writer（PUB）----------------

Result<std::shared_ptr<WriterHandle>> CreateWriterCore(
    const ChannelConfig& config, const std::shared_ptr<InjectionContext>& ctx);
WriteStatus WriterWriteBytes(WriterHandle* handle, const std::string& bytes);
bool WriterHasReader(const WriterHandle* handle);
EndpointStats WriterStats(const WriterHandle* handle);
void WriterShutdown(WriterHandle* handle);

// 传输层 send 返回码 → WriteStatus（供单测验证映射，不依赖真实拥塞）
WriteStatus MapSendError(int r) noexcept;

// ---------------- Reader（SUB）----------------

// 类型擦除的消息处理函数：core 收到字节帧后由模板壳反序列化并调用业务回调。
using OnBytesFunc =
    std::function<CallbackResult(const MessageContext&, const std::string&)>;

Result<std::shared_ptr<ReaderHandle>> CreateReaderCore(
    const ReaderConfig& config, OnBytesFunc on_bytes,
    const std::shared_ptr<InjectionContext>& ctx);
void ReaderShutdownCore(ReaderHandle* handle);
void ReaderClearData(ReaderHandle* handle);
bool ReaderEmpty(const ReaderHandle* handle);
bool ReaderHasReceived(const ReaderHandle* handle);
double ReaderGetDelaySec(const ReaderHandle* handle);
std::vector<std::string> ReaderGetAllBytes(const ReaderHandle* handle);
void ReaderSetHistoryDepth(ReaderHandle* handle, std::uint32_t depth);
std::uint32_t ReaderGetHistoryDepth(const ReaderHandle* handle);
bool ReaderHasWriter(const ReaderHandle* handle);
EndpointStats ReaderStats(const ReaderHandle* handle);
// Clear 等待在途回调：同 Reader 内与收线程 join 配合；供全局 Clear 汇总等待。
void ReaderWaitForCallbacks(ReaderHandle* handle);

// ---------------- Service（REP）----------------

// 类型擦除的业务处理：模板壳负责反序列化请求/序列化响应，core 只管帧收发。
using OnRequestFunc = std::function<std::string(const std::string& req_bytes,
                                                ErrorCode* code)>;

Result<std::shared_ptr<ServiceHandle>> CreateServiceCore(
    const std::string& node_name, const std::string& service_name,
    OnRequestFunc on_request, const std::shared_ptr<InjectionContext>& ctx);
void ServiceShutdownCore(ServiceHandle* handle);
EndpointStats ServiceStats(const ServiceHandle* handle);

// ---------------- Client（REQ）----------------

// ---- 第 1 批：RPC 队列与取消契约 ----
// 同步 ClientSendBytes 与异步 ClientAsyncSendBytes 共用 ClientCore 内同一个
// 有界队列（64 项）和单个 I/O 执行者；不在调用线程做 I/O。入队时以
// steady_clock 计算绝对 deadline，排队、服务查询、连接、发送、接收共用该
// 预算，任一步耗尽即 kTimeout；队列满以现有错误码 kQueueFull 失败。
// 取消两态：执行者出队后、Send 前用同一状态转换裁决。
//   - 排队中取消：请求从发送路径移除，服务端执行 0 次；
//   - 已提交发送后取消：只结束本地等待并兑现 kCancelled，服务端仍可能执行。
// 每请求只兑现一次；Shutdown/析构取消全部在途并停止执行者，旧请求句柄
// Cancel() 返回 false。公共 API 签名不变。
//
// 一次性兑现回调：成功后 payload 为响应字节；失败时忽略 payload。
using OnSettledFunc = std::function<void(ErrorCode code, const std::string& payload)>;

struct WireResponse {
  ErrorCode code = ErrorCode::kTransport;
  std::string payload;
};

Result<std::shared_ptr<ClientHandle>> CreateClientCore(
    const std::string& node_name, const std::string& service_name,
    const std::shared_ptr<InjectionContext>& ctx);
WireResponse ClientSendBytes(ClientHandle* handle, const std::string& bytes,
                             std::chrono::milliseconds timeout);
std::uint64_t ClientAsyncSendBytes(ClientHandle* handle,
                                   const std::string& bytes,
                                   std::chrono::milliseconds timeout,
                                   OnSettledFunc on_settled);
bool ClientCancelRequestCore(ClientHandle* handle, std::uint64_t id);
void ClientShutdownCore(ClientHandle* handle);
bool ClientServiceIsReady(const ClientHandle* handle);
// nullopt = 无限等待；0ms = 只检查当前状态（语义与阶段二负数魔法值一致）。
bool ClientWaitForService(const ClientHandle* handle,
                          std::optional<std::chrono::milliseconds> timeout);
EndpointStats ClientStats(const ClientHandle* handle);

}  // namespace detail
}  // namespace firefly

#endif  // FIREFLY_DETAIL_ENDPOINT_API_H_
