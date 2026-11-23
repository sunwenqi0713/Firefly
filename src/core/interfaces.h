#ifndef FIREFLY_CORE_INTERFACES_H_
#define FIREFLY_CORE_INTERFACES_H_

// 内部桥接头：Core 与后端共享的三组契约。
// 按关注点拆分为 transport.h / registry.h / injection.h；本头只为减少
// 现有 .cc 与测试的 include 改动而保留。公共模板不包含本头。
#include "core/injection.h"
#include "core/registry.h"
#include "core/transport.h"

#endif  // FIREFLY_CORE_INTERFACES_H_
