#ifndef FIREFLY_DETAIL_ENDPOINT_API_H_
#define FIREFLY_DETAIL_ENDPOINT_API_H_

// ===========================================================================
// 端点的非模板传输 API（detail 层，阶段三任务 2）
// ===========================================================================
// 本头文件不含 nng、不含任何 *_impl.h。模板端点（Writer/Reader/Service/
// Client）只做 protobuf 序列化 shim，所有 socket/dial/线程/统计逻辑都在
// 对应的 *_core.cc 中实现。用户自定义 protobuf 消息无需显式实例化。
// 端点方法签名与阶段一/二的语义（WriteStatus/ErrorCode/Cancel）保持不变。
//
// ===========================================================================
// 第 2 批：数据传输角色契约（Core ↔ 适配器）
// ---------------------------------------------------------------------------
// 四个 Core 只依赖 interfaces.h 中的 Publisher/Subscriber/RpcRequester/
// RpcResponder 及其内部工厂，不再依赖 socket 类型、连接/重连细节、NS 错误码
// 或超时 setsockopt。以下四张表是唯一的行为契约；实现（transport_nng.cc 与
// 测试假后端）必须逐条满足。
//
// 统一传输状态 TransferStatus（定义在 interfaces.h）：
//   kOk           本地操作成功；对 Exchange 表示完整收到一帧响应。
//   kTimeout      绝对截止时间已耗尽；调用方映射 kTimeout 并按需重置连接。
//   kClosed       端点已 Close 或不可用；调用方停止接纳并退出 I/O 循环。
//   kWouldBlock   后端队列/发送窗口满；Core 映射 WriteStatus::kQueueFull /
//                 ErrorCode::kQueueFull。
//   kIoError      其他后端错误；Core 映射为 kTransportError / kTransport。
//   kInvalidArgument 参数非法；Core 映射为 kTransport（detail 层内部错误）。
//
// Publisher（发布者，Core = WriterCore）
// | 操作 | 契约 | Core 保留的职责 |
// | Bind(std::string* endpoint) | 绑定本地监听端点并写回不透明地址串；失败返回非 kOk。 | 注册中心登记地址、日志与 Result 文案。 |
// | Send(bytes) | 按消息提交一帧；不保证对端已收到。kWouldBlock 仅表示本地发送窗口满。 | WriteStatus 映射、kNoSubscriber 判定（查注册中心）、统计。 |
// | Close() | 幂等；置位后所有 Send 返回 kClosed；可跨线程调用并唤醒阻塞 I/O。 | 停止接纳后调用，随后注销并回收。 |
//
// Subscriber（订阅者，Core = ReaderCore）
// | 操作 | 契约 | Core 保留的职责 |
// | Connect(endpoint) | 把发布者地址交给后端；后端自行去重/建连/重连。 | 发现事件、地址集合传递、HasWriter。 |
// | Receive(bytes, deadline) | 收绝对截止时间；每次阻塞前重算剩余预算，kTimeout 只表示本次等待到期，可继续下一轮。Close 唤醒并使本调用返回 kClosed。 | 运行标志、回调串行、Observe 缓冲、统计。 |
// | Close() | 幂等、跨线程唤醒阻塞 Receive；之后所有 Receive 立即返回 kClosed。 | 停止接收 → 唤醒 → join → 注销。 |
//
// RpcRequester（请求方，Core = ClientCore）
// | 操作 | 契约 | Core 保留的职责 |
// | Exchange(endpoint, request, deadline, response) | 一次完整请求应答交换；绝对 deadline 由 Core 入队时确定并原样传入。连接/重连、发送、接收各阻塞点之前都重算剩余时间；任一步耗尽即 kTimeout。响应帧字节原样写回 response，适配器不解析业务 code。 | 有界队列、单执行者、PendingCall 状态机、发送前取消裁决、帧解码与结果兑现。 |
// | Close() | 幂等、跨线程唤醒阻塞 Exchange；之后 Exchange 立即返回 kClosed。 | 关停顺序与 pending 回收。 |
//
// RpcResponder（应答方，Core = ServiceCore）
// | 操作 | 契约 | Core 保留的职责 |
// | Bind(std::string* endpoint) | 绑定本地监听端点并写回地址串。 | 注册中心登记、日志。 |
// | Receive(request, ReplyToken*, deadline) | 收一帧请求；成功时适配器生成不透明 token 交回，token 与本次请求一一对应。绝对 deadline 在每次阻塞前重算。 | 调用业务回调、统计、串行回调约束。 |
// | Reply(token, frame, deadline) | 适配器消费 token 并把已编码的响应帧写回原请求；token 只能使用一次，错误/超时也必须用同一 token 应答或丢弃。 | 编码帧、业务错误码映射；不解析 token。 |
// | Close() | 幂等、跨线程唤醒阻塞 Receive；之后所有 Receive/Reply 返回 kClosed。 | 停止接收 → 唤醒 → join → 注销。 |
//
// ReplyToken 不透明约定：它是 interfaces.h 中只有虚析构函数的空接口；具体
// 类型由适配器定义并全部留在适配器内部。Core 只把 Receive 产出的 unique_ptr
// 原样交给 Reply（或让它在超时路径析构），不得 dynamic_cast、不得读取内部
// 字段、不得跨请求复用、不得凭 token 内容做控制流判断。
// Close() 对所有角色都是幂等且可从另一线程调用：调用返回后，任何仍在阻塞的
// I/O 都已被唤醒并以 kClosed 返回；此后所有新调用立即返回 kClosed。
// Exchange 的 deadline 是 std::chrono::steady_clock::time_point，非相对时长；
// 适配器不得用传入值重新加超时，也不得在不同步骤共用一次算出的剩余毫秒数。
// ===========================================================================
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

// 传输状态 → WriteStatus（供单测验证映射，不依赖真实拥塞）
WriteStatus MapSendError(TransferStatus status) noexcept;

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
