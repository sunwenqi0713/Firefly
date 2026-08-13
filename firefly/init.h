#ifndef FIREFLY_INIT_H_
#define FIREFLY_INIT_H_

namespace firefly {

// 初始化框架（初始化日志、调度器、拓扑发现、传输层等）
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