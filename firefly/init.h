#ifndef FIREFLY_INIT_H_
#define FIREFLY_INIT_H_

#include <chrono>

namespace firefly {

// ===========================================================================
// 初始化与就绪语义（显式契约，阶段一；关停语义阶段二补全）
// ===========================================================================
// Init() 完成时，注册中心**尚未就绪**：当前实现为惰性启动 —— 首次
// Register/List/Subscribe（由 CreateWriter/CreateReader/CreateService 触发）
// 时才真正 bind/dial（见 init.cc）。因此 Init() 返回 true 不代表拓扑发现
// 可用；客户端应显式调用 Client::WaitForService() 等待服务就绪。
//
// binary_name 参数当前未使用（runtime.cc 中 (void)binary_name），保留以兼容
// 现有签名。Init 幂等：重复调用返回 true。
//
// ---- 关停语义（Shutdown / WaitForShutdown / WaitForShutdownFor / Clear） ----
// Shutdown()：主动触发全局关停，复用 ShutdownManager 的 shutdown_ 状态位
//   （与 Ctrl+C 信号同一路径），幂等。触发后的可观察效果（如实描述现状）：
//     1) WaitForShutdown() 解除阻塞并返回；IsShutdown()==true；OK()==false。
//     2) 此后 Create* 工厂返回 ErrorCode::kNotReady（阶段一已有的 OK() 检查）。
//     3) 已创建端点：Shutdown() **只置全局标志，不主动关闭任何端点 socket、
//        不停止端点线程**。Writer::Write 仍会尝试发送到已绑定的 socket；
//        Reader/Service 的收/工作线程循环只看各自 running_ 标志，不看全局
//        shutdown_，故回调**仍会继续触发**，直到该端点被单独 Shutdown/析构。
//        Registry 的 REP/SUB 线程例外：其循环检查 IsShutdown()，会退出。
//        结论：全局 Shutdown() 不是"停止所有通信"，调用方须自行 Shutdown
//        或析构各端点。全局关停级联关闭所有端点仍属未来机制，不在本阶段范围。
// WaitForShutdownFor(timeout)：带期限等待，false=超时仍未关停。
// Clear()：调用 Registry::Stop() 并置 inited_=false（间接 RequestShutdown）。
//   - 与在途回调：Clear() 先停 Registry，再遍历所有存活端点登记的等待点，
//     阻塞至**调用时刻已在途**的 Reader/Service 回调返回（阶段三已实现）。
//     边界：全局关停并不关闭端点线程，Clear() 返回后新到达的消息仍可能触发
//     新回调；调用方若要彻底停止回调，需单独 Shutdown/析构对应端点。
//     Service 回调等待同样通过等待点登记；Reader::Shutdown() 自身也会等待。
//   - Clear() 后可再次 Init()：state 机为 inited_/shutdown_ 两原子位，Clear()
//     将 inited_ 置 false 并 RequestShutdown()；再次 Init() 把 inited_ 置回
//     true、shutdown_ 置回 false（见 runtime.cc Init）。注册中心为惰性启动，
//     下次 Register/List 时重新 start。
// ===========================================================================

// 初始化框架（初始化日志、调度器、关停管理器；注册中心惰性启动）
bool Init(const char* binary_name);

// 框架是否运行正常
bool OK();

// 是否已进入关停流程
bool IsShutdown();

// 主动触发全局关停（幂等；等价于信号触发的 RequestShutdown）
void Shutdown();

// 阻塞等待框架关停
void WaitForShutdown();

// 带超时等待框架关停：true=已关停，false=超时
bool WaitForShutdownFor(std::chrono::milliseconds timeout);

// 清理并释放框架资源
void Clear();

}  // namespace firefly

#endif  // FIREFLY_INIT_H_