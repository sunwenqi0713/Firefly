#ifndef FIREFLY_DETAIL_BACKEND_ACCESS_H_
#define FIREFLY_DETAIL_BACKEND_ACCESS_H_

// ===========================================================================
// 后端显式构造入口（detail 层，第 4 批）
// ===========================================================================
// 这些函数只用于后端无关的契约测试与构建期组装检查；公共 API 不依赖它们。
// 每个后端 object 集合只导出自己那一份，最终链接中任何符号只出现一次。
#include <memory>

#include "core/interfaces.h"

namespace firefly {
namespace detail {

// 内存后端：始终可用（Core object 集合的一部分）。
std::shared_ptr<IEndpointFactory> MakeMemoryEndpointFactory();
std::shared_ptr<IRegistry> MakeMemoryRegistry();

// NNG 后端：只在 FIREFLY_WITH_NNG=ON 时由 nng backend object 导出。
#if FIREFLY_WITH_NNG
std::shared_ptr<IEndpointFactory> MakeNngEndpointFactory();
std::shared_ptr<IRegistry> MakeNngRegistry();
#endif

}  // namespace detail
}  // namespace firefly

#endif  // FIREFLY_DETAIL_BACKEND_ACCESS_H_
