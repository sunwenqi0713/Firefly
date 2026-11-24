# 模块边界与依赖

源码按「公共头 / Core / 后端 / 运行时」四层分布，依赖方向单向向下。本文只描述当前结构；构建命令与开关见 [guides/build.md](../guides/build.md)。

## 1. 目录与职责

| 模块 | 路径 | 职责 |
| --- | --- | --- |
| 公共 API | [include/firefly/](../../include/firefly/) | `Node`、读写端点、RPC 端点、消息 traits、错误类型。模板经 [endpoint_api.h](../../include/firefly/detail/endpoint_api.h) 调用类型擦除的 Core |
| Core | [src/core/](../../src/core/) | 准人、缓存、请求队列、回调派发、错误映射、资源释放。传输入口 [transport.h](../../src/core/transport.h)，发现入口 [registry.h](../../src/core/registry.h) |
| 后端 | [src/backends/nng/](../../src/backends/nng/)、[src/backends/memory/](../../src/backends/memory/) | 分别实现四角色与发现。NNG 地址、socket 操作、协议状态与原始返回码归对应后端 |
| 运行时 | [src/runtime/](../../src/runtime/) | 进程初始化、关停信号、全局清理。[InjectionContext](../../src/core/injection.h) 携带工厂与 Registry，缺省字段由默认注入补齐 |
| 测试 | [tests/](../../tests/) | `contract/` 行为契约、`integration/` 端到端与跨进程、`support/` 共享 `.proto` 与消息 |
| 工具 | [scripts/](../../scripts/)、[tools/](../../tools/) | `verify.sh` 验收、`check_no_leak.py` 公共头泄漏检查 |

## 2. 依赖约束

- 公共头只依赖公共 include 根与必要的公共第三方头（protobuf）。**不得**出现 `<nng/...>`、后端类型或 `src/` 私有头。
- Core 只通过角色接口与 `IRegistry` 使用后端能力，源码中不得出现后端专有类型、常量与错误码。
- 后端依赖由对应 CMake target 承载；`FIREFLY_WITH_NNG=OFF` 时可只用 Protobuf 构建与链接。
- 应用链接 `firefly::firefly`，使用 `#include <firefly/firefly.h>`。

这两条由 `no_leak_check`（`tools/check_no_leak.py`）与 `header_check` 守护，改动公共头必须跑（见 [guides/testing.md](../guides/testing.md)）。

## 3. target 与目录对应

| CMake target | 组成 | 依赖 |
| --- | --- | --- |
| `firefly_core_objects`（OBJECT） | `src/core/*_core.cc`、`node.cc`、`endpoint_id.cc`、`log.cc`、`rpc_frame.h`、`src/runtime/*.cc` | protobuf、Threads |
| `firefly_memory_role_objects`（OBJECT） | `src/backends/memory/{transport,registry}.cc` | protobuf、Threads |
| `firefly_memory_injection_objects`（OBJECT） | `src/backends/memory/injection.cc` | 同上 |
| `firefly_nng_role_objects`（OBJECT，仅 ON） | `src/backends/nng/{transport,registry}.cc`、`net.h` | `nng::nng` |
| `firefly_nng_injection_objects`（OBJECT，仅 ON） | `src/backends/nng/injection.cc` | `nng::nng` |
| `firefly_core`（STATIC） | Core object + 内存角色 object + 内存注入 object | 无 NNG，可独立检查 |
| `firefly_nng`（STATIC，仅 ON） | NNG 角色 object + NNG 注入 object | `nng::nng` |
| `firefly` / `firefly::firefly` | Core object + **一个**后端的角色与注入 object | 默认 ON 选 NNG，OFF 选内存 |

拆成「角色 object」与「注入 object」两层的唯一原因是**注入符号每个后端各一份、最终链接只能出现一份**，而角色 object 需要被同一测试进程显式链接多次（共享契约测试同时构造内存与 NNG 实例）。新增后端时沿用这一结构，见 [guides/adding-backend.md](../guides/adding-backend.md)。

`configure_firefly_object()` 统一设置 include 根（`include/`、`src/`）与编译选项；目标由 `FIREFLY_BACKEND_OBJECTS` 决定聚合库取哪一套。

## 4. 新增文件的落位规则

| 要加的东西 | 放哪里 | 不得放 |
| --- | --- | --- |
| 新公开端点或公共类型 | `include/firefly/{node,service,common}/` | `src/`（应用无法包含） |
| 端点行为、队列、派发逻辑 | `src/core/<name>_core.cc` | 后端目录 |
| 后端特有能力（socket、协议、地址解析） | `src/backends/<name>/` | `src/core/` |
| 类型擦除的跨界声明 | `src/core/endpoint_api.h` / `backend_access.h` | 公共头 |
| 契约测试 / 端到端 / 跨进程辅助 | `tests/{contract,integration,support}/` | `src/` |

跨层新增入口时，必须在 [contracts.md](contracts.md) 或对应指南中同步契约，不允许只在代码里体现。
