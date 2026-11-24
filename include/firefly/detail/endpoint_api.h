#ifndef FIREFLY_DETAIL_ENDPOINT_API_H_
#define FIREFLY_DETAIL_ENDPOINT_API_H_

/// @file endpoint_api.h
/// @brief 端点的非模板传输 API 与适配器契约。
///
/// 本文件属于 detail 层：业务代码只应通过 Node 与四个模板端点使用本库，
/// 不应直接调用这里的函数。类型擦除模板只做 protobuf 序列化，socket、线程、
/// 统计等逻辑都在对应的 Core 实现中。
///
/// @par 适配器不可打折项
/// 以下四张表是后端实现的唯一行为契约，任何实现都必须逐条满足。
/// 架构层面的总览见 docs/architecture/contracts.md §7。
///
/// 统一传输状态 TransferStatus（完整定义见 src/core/transport.h）：
/// - kOk：本地操作成功；对 Exchange 表示完整收到一帧响应。
/// - kTimeout：绝对截止时间已耗尽；调用方映射 kTimeout 并按需重置连接。
/// - kClosed：端点已关闭或不可用；调用方停止接纳并退出 I/O 循环。
/// - kWouldBlock：后端队列或发送窗口已满；映射为 WriteStatus::kQueueFull 或
///   ErrorCode::kQueueFull。
/// - kIoError：其他后端错误；映射为 kTransportError 或 kTransport。
/// - kInvalidArgument：参数非法；映射为 kTransport。
///
/// @par Publisher，Core 为 WriterCore
/// | 操作 | 契约 | Core 保留的职责 |
/// | --- | --- | --- |
/// | Bind(std::string* endpoint) | 绑定本地监听端点并写回不透明地址串；失败返回非 kOk。 | 注册中心登记地址、日志与 Result 文案。 |
/// | Send(bytes) | 按消息提交一帧；不保证对端已收到。kWouldBlock 仅表示本地发送窗口满。 | WriteStatus 映射、kNoSubscriber 判定、统计。 |
/// | Close() | 幂等；置位后所有 Send 返回 kClosed；可跨线程调用并唤醒阻塞 I/O。 | 停止接纳后调用，随后注销并回收。 |
///
/// @par Subscriber，Core 为 ReaderCore
/// | 操作 | 契约 | Core 保留的职责 |
/// | --- | --- | --- |
/// | UpdatePublishers(endpoints) | 收发现层给出的当前地址全集；适配器自行去重、建连、断开已移除地址；重复同一集合为 no-op，空串忽略。 | Watch 快照转地址集合、HasWriter。 |
/// | Receive(bytes, deadline) | 收绝对截止时间；每次阻塞前重算剩余预算，kTimeout 只表示本次等待到期，可继续下一轮。Close 唤醒并使本调用返回 kClosed。 | 运行标志、回调串行、Observe 缓冲、统计。 |
/// | Close() | 幂等、跨线程唤醒阻塞 Receive；之后所有 Receive 立即返回 kClosed。 | 停止接收、唤醒、join、注销。 |
///
/// @par RpcRequester，Core 为 ClientCore
/// | 操作 | 契约 | Core 保留的职责 |
/// | --- | --- | --- |
/// | Exchange(endpoint, request, deadline, response) | 一次完整请求应答交换；绝对 deadline 由 Core 入队时确定并原样传入。连接、发送、接收各阻塞点之前都重算剩余时间；任一步耗尽即 kTimeout。响应帧字节原样写回 response，适配器不解析业务 code。 | 有界队列、单执行者、请求状态机、发送前取消裁决、帧解码与结果兑现。 |
/// | Close() | 幂等、跨线程唤醒阻塞 Exchange；之后 Exchange 立即返回 kClosed。 | 关停顺序与在途请求回收。 |
///
/// @par RpcResponder，Core 为 ServiceCore
/// | 操作 | 契约 | Core 保留的职责 |
/// | --- | --- | --- |
/// | Bind(std::string* endpoint) | 绑定本地监听端点并写回地址串。 | 注册中心登记、日志。 |
/// | Receive(request, ReplyToken*, deadline) | 收一帧请求；成功时适配器生成不透明 token 交回，token 与本次请求一一对应。绝对 deadline 在每次阻塞前重算。 | 调用业务回调、统计、串行回调约束。 |
/// | Reply(token, frame, deadline) | 适配器消费 token 并把已编码的响应帧写回原请求；token 只能使用一次，错误或超时也必须用同一 token 应答或丢弃。 | 编码帧、业务错误码映射；不解析 token。 |
/// | Close() | 幂等、跨线程唤醒阻塞 Receive；之后所有 Receive 与 Reply 返回 kClosed。 | 停止接收、唤醒、join、注销。 |
///
/// @par 时序与不透明约定
/// - Writer 与 Service 必须先完成 Bind（拿到监听地址）才 Register；Reader 无
///   监听地址，以空 address 独立登记。
/// - 四个 Core 都在启动后台线程之前完成注册；Register 或 Watch 失败时立即
///   关闭已创建的端点并返回 kTransport，不留下线程或监听器。
/// - 注销一律使用注册返回的 EndpointRecordId，同一 id 只注销一次。
/// - 全局 Clear() 经默认注入面停止注册中心并清空记录；此后旧端点析构触发的
///   迟到注销是 no-op。
/// - ReplyToken 是只有虚析构函数的空接口，具体类型由适配器定义。Core 只把它
///   原样交回 Reply，不得转换、不得读字段、不得跨请求复用。
/// - Close() 对所有角色都幂等且可跨线程调用：返回后任何仍在阻塞的 I/O 都已
///   被唤醒并以 kClosed 返回，此后新调用立即返回 kClosed。
/// - Exchange 的 deadline 是绝对时间点，不是相对时长。

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "firefly/common/types.h"
#include "firefly/detail/handles.h"
#include "firefly/node/channel_config.h"

namespace firefly {
namespace detail {

/// @brief 传输层统一状态；完整定义见 src/core/transport.h。
enum class TransferStatus;

/// @brief 端点依赖的注入点；定义见 src/core/injection.h。
struct InjectionContext;

// ---------------- Writer ----------------

/// @brief 创建发布端点的内部实现。
/// @param[in] config 通道名与 QoS。
/// @param[in] ctx    注入点；为空时使用默认后端与默认注册中心。
/// @return 成功时 value 为句柄；失败时 code 指明原因。
Result<std::shared_ptr<WriterHandle>> CreateWriterCore(
    const ChannelConfig& config, const std::shared_ptr<InjectionContext>& ctx);

/// @brief 提交一帧字节。
/// @param[in] handle 发布端点句柄。
/// @param[in] bytes  已序列化的消息字节。
/// @return 本次写入的受理结果，语义与 Writer::Write() 一致。
WriteStatus WriterWriteBytes(WriterHandle* handle, const std::string& bytes);

/// @brief 查询通道当前是否有 Reader。
/// @param[in] handle 发布端点句柄。
/// @return 有已注册的 Reader 时返回 true；句柄为空或已关闭时返回 false。
bool WriterHasReader(const WriterHandle* handle);

/// @brief 取发布端点的累计统计。
/// @param[in] handle 发布端点句柄；为空时返回全零。
EndpointStats WriterStats(const WriterHandle* handle);

/// @brief 关闭发布端点；幂等。
/// @param[in] handle 发布端点句柄；为空时无操作。
void WriterShutdown(WriterHandle* handle);

/// @brief 把传输状态映射为 WriteStatus，供单元测试验证映射而不依赖真实拥塞。
/// @param[in] status 传输层状态。
/// @return 对应的写入结果。
WriteStatus MapSendError(TransferStatus status) noexcept;

// ---------------- Reader ----------------

/// @brief 类型擦除的消息处理函数：Core 收到字节帧后由模板壳反序列化并回调。
using OnBytesFunc =
    std::function<CallbackResult(const MessageContext&, const std::string&)>;

/// @brief 创建订阅端点的内部实现。
/// @param[in] config   通道名与 QoS。
/// @param[in] on_bytes 消息处理函数；为空表示只入队不回调。
/// @param[in] ctx      注入点；为空时使用默认后端与默认注册中心。
/// @return 成功时 value 为句柄；失败时 code 指明原因。
Result<std::shared_ptr<ReaderHandle>> CreateReaderCore(
    const ReaderConfig& config, OnBytesFunc on_bytes,
    const std::shared_ptr<InjectionContext>& ctx);

/// @brief 关闭订阅端点，停止收线程并等待在途回调返回；幂等。
/// @param[in] handle 订阅端点句柄；为空时无操作。
void ReaderShutdownCore(ReaderHandle* handle);

/// @brief 清空观察队列。
/// @param[in] handle 订阅端点句柄。
void ReaderClearData(ReaderHandle* handle);

/// @brief 观察队列是否为空。
/// @param[in] handle 订阅端点句柄。
bool ReaderEmpty(const ReaderHandle* handle);

/// @brief 自上次清空后是否收到过消息。
/// @param[in] handle 订阅端点句柄。
bool ReaderHasReceived(const ReaderHandle* handle);

/// @brief 距最近一次收到消息的时间间隔。
/// @param[in] handle 订阅端点句柄。
/// @return 秒数；从未收到过返回 -1。
double ReaderGetDelaySec(const ReaderHandle* handle);

/// @brief 取观察队列中的全部原始字节，按到达顺序排列。
/// @param[in] handle 订阅端点句柄。
std::vector<std::string> ReaderGetAllBytes(const ReaderHandle* handle);

/// @brief 设置观察队列深度。
/// @param[in] handle 订阅端点句柄。
/// @param[in] depth  保留的最近条数。
void ReaderSetHistoryDepth(ReaderHandle* handle, std::uint32_t depth);

/// @brief 取观察队列深度。
/// @param[in] handle 订阅端点句柄。
std::uint32_t ReaderGetHistoryDepth(const ReaderHandle* handle);

/// @brief 通道当前是否有 Writer。
/// @param[in] handle 订阅端点句柄。
bool ReaderHasWriter(const ReaderHandle* handle);

/// @brief 取订阅端点的累计统计。
/// @param[in] handle 订阅端点句柄；为空时返回全零。
EndpointStats ReaderStats(const ReaderHandle* handle);

/// @brief 等待在途回调返回，供全局 Clear 汇总等待。
/// @param[in] handle 订阅端点句柄。
void ReaderWaitForCallbacks(ReaderHandle* handle);

// ---------------- Service ----------------

/// @brief 类型擦除的业务处理：模板壳反序列化请求并序列化响应，Core 只管帧收发。
/// @note 实现返回响应字节，并通过 code 回写业务错误码。
using OnRequestFunc = std::function<std::string(const std::string& req_bytes,
                                                ErrorCode* code)>;

/// @brief 创建服务端点的内部实现。
/// @param[in] node_name    所属节点名，用于注册与日志。
/// @param[in] service_name 服务名。
/// @param[in] on_request   业务处理函数。
/// @param[in] ctx          注入点；为空时使用默认后端与默认注册中心。
/// @return 成功时 value 为句柄；失败时 code 指明原因。
Result<std::shared_ptr<ServiceHandle>> CreateServiceCore(
    const std::string& node_name, const std::string& service_name,
    OnRequestFunc on_request, const std::shared_ptr<InjectionContext>& ctx);

/// @brief 关闭服务端点，停止受理并等待在途回调返回；幂等。
/// @param[in] handle 服务端点句柄；为空时无操作。
void ServiceShutdownCore(ServiceHandle* handle);

/// @brief 取服务端点的累计统计。
/// @param[in] handle 服务端点句柄；为空时返回全零。
EndpointStats ServiceStats(const ServiceHandle* handle);

// ---------------- Client ----------------

/// @brief 一次性兑现回调：成功时 payload 为响应字节，失败时忽略 payload。
using OnSettledFunc = std::function<void(ErrorCode code, const std::string& payload)>;

/// @brief 请求应答的字节级结果。
struct WireResponse {
  ErrorCode code = ErrorCode::kTransport;  ///< 结果码；kOk 时 payload 有效。
  std::string payload;                     ///< 响应字节。
};

/// @brief 创建客户端端点的内部实现。
/// @param[in] node_name    所属节点名。
/// @param[in] service_name 目标服务名。
/// @param[in] ctx          注入点；为空时使用默认后端与默认注册中心。
/// @return 成功时 value 为句柄；失败时 code 指明原因。
Result<std::shared_ptr<ClientHandle>> CreateClientCore(
    const std::string& node_name, const std::string& service_name,
    const std::shared_ptr<InjectionContext>& ctx);

/// @brief 同步执行一次请求应答交换，阻塞至收到响应或超时。
/// @param[in] handle  客户端句柄。
/// @param[in] bytes   已序列化的请求字节。
/// @param[in] timeout 端到端预算，从入队时刻起算。
/// @return 结果码与响应字节；失败时 payload 无意义。
WireResponse ClientSendBytes(ClientHandle* handle, const std::string& bytes,
                             std::chrono::milliseconds timeout);

/// @brief 异步提交一次请求应答交换。
/// @param[in] handle     客户端句柄。
/// @param[in] bytes      已序列化的请求字节。
/// @param[in] timeout    端到端预算，从入队时刻起算。
/// @param[in] on_settled 兑现回调，恰好被调用一次。
/// @return 请求标识，可用于 ClientCancelRequestCore()；句柄为空时返回 0。
std::uint64_t ClientAsyncSendBytes(ClientHandle* handle,
                                   const std::string& bytes,
                                   std::chrono::milliseconds timeout,
                                   OnSettledFunc on_settled);

/// @brief 取消一个在途请求。
/// @param[in] handle 客户端句柄。
/// @param[in] id     请求标识。
/// @return 取消获胜并已兑现时返回 true；请求已完成、已取消或句柄无效时返回 false。
bool ClientCancelRequestCore(ClientHandle* handle, std::uint64_t id);

/// @brief 关闭客户端，取消全部在途请求并等待 I/O 执行者退出；幂等。
/// @param[in] handle 客户端句柄；为空时无操作。
void ClientShutdownCore(ClientHandle* handle);

/// @brief 目标服务当前是否可被发现。
/// @param[in] handle 客户端句柄。
bool ClientServiceIsReady(const ClientHandle* handle);

/// @brief 等待目标服务变为可被发现。
/// @param[in] handle  客户端句柄。
/// @param[in] timeout 等待上限；nullopt 表示无限等待，0ms 表示只检查当前状态。
/// @return 在期限内观察到服务时返回 true。
bool ClientWaitForService(const ClientHandle* handle,
                          std::optional<std::chrono::milliseconds> timeout);

/// @brief 取客户端端点的累计统计。
/// @param[in] handle 客户端句柄；为空时返回全零。
EndpointStats ClientStats(const ClientHandle* handle);

}  // namespace detail
}  // namespace firefly

#endif  // FIREFLY_DETAIL_ENDPOINT_API_H_
