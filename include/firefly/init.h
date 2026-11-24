#ifndef FIREFLY_INIT_H_
#define FIREFLY_INIT_H_

/// @file init.h
/// @brief 框架初始化、就绪判定与关停接口。

#include <chrono>

namespace firefly {

/**
 * @brief 初始化框架：日志、调度与关停管理。
 *
 * @param[in] binary_name 可执行文件名；当前保留但未使用，传 argv[0] 即可。
 * @return 当前实现恒返回 true，且幂等：重复调用不会再做任何组装。不要用返回值
 *         判断框架是否可用，应使用 OK()。
 * @post Init() 返回后注册中心**尚未就绪**：它为惰性启动，首次 Register、List
 *       或 Subscribe（由 CreateWriter、CreateReader、CreateService 触发）时才
 *       真正建立连接。因此 Init() 成功不代表拓扑发现可用，客户端应显式调用
 *       Client::WaitForService() 等待服务就绪。
 * @note 幂等：重复调用直接返回 true。发现服务地址可在首次使用前通过
 *       detail::ConfigureDiscovery() 或环境变量 FIREFLY_DISCOVERY_REP_URL、
 *       FIREFLY_DISCOVERY_PUB_URL 配置；测试可通过注入点替换端点实现。
 */
bool Init(const char* binary_name);

/// @brief 框架当前是否正常运行（已 Init 且未进入关停流程）。
bool OK();

/// @brief 是否已进入关停流程。
bool IsShutdown();

/**
 * @brief 主动触发全局关停，与信号触发的关停走同一路径。
 *
 * @post WaitForShutdown() 解除阻塞，IsShutdown() 为 true，OK() 为 false；
 *       此后所有 Create 工厂返回 kNotReady。
 * @warning 本调用只置全局标志，不关闭任何已创建的端点，也不停止端点线程：
 *          Writer 仍会向已绑定的端点发送，Reader 与 Service 的回调仍会继续
 *          触发，直到该端点被单独 Shutdown() 或析构。需要停止通信时，调用方
 *          必须自行关闭各端点。详见 docs/architecture/known-gaps.md。
 * @note 幂等。
 */
void Shutdown();

/// @brief 阻塞等待框架进入关停状态。
void WaitForShutdown();

/**
 * @brief 带期限等待框架进入关停状态。
 *
 * @param[in] timeout 等待上限。
 * @return 已关停返回 true；超时未关停返回 false。
 */
bool WaitForShutdownFor(std::chrono::milliseconds timeout);

/**
 * @brief 清理框架资源，允许之后重新 Init()。
 *
 * 先停止注册中心，再遍历全部存活端点登记的等待点，阻塞至**调用时刻已在途**
 * 的 Reader 与 Service 回调返回。
 *
 * @post inited_ 置为 false 并触发关停；再次 Init() 会重建组装。
 * @warning 全局关停不关闭端点线程，本调用返回后新到达的消息仍可能触发新回调。
 *          要彻底停止回调，须单独 Shutdown() 或析构对应端点。
 * @note 端点析构时触发的迟到注销在此之后为 no-op。
 */
void Clear();

}  // namespace firefly

#endif  // FIREFLY_INIT_H_