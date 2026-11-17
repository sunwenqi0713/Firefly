#ifndef FIREFLY_INIT_H_
#define FIREFLY_INIT_H_

namespace firefly {

// ===========================================================================
// 初始化与就绪语义（显式契约，阶段一）
// ===========================================================================
// Init() 完成时，注册中心**尚未就绪**：当前实现为惰性启动 —— 首次
// Register/List/Subscribe（由 CreateWriter/CreateReader/CreateService 触发）
// 时才真正 bind/dial（见 init.cc）。因此 Init() 返回 true 不代表拓扑发现
// 可用；客户端应显式调用 Client::WaitForService() 等待服务就绪。
//
// binary_name 参数当前未使用（runtime.cc 中 (void)binary_name），保留以兼容
// 现有签名。Init 幂等：重复调用返回 true。Clear() 后 OK() 返回 false，
// 再次 Init() 可重新启动运行时（回归路径，本阶段未额外约束）。
// ===========================================================================

// 初始化框架（初始化日志、调度器、关停管理器；注册中心惰性启动）
bool Init(const char* binary_name);

// 框架是否运行正常
bool OK();

// 是否已进入关停流程
bool IsShutdown();

// 阻塞等待框架关停
void WaitForShutdown();

// 清理并释放框架资源
void Clear();

}  // namespace firefly

#endif  // FIREFLY_INIT_H_