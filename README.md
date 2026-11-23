# Firefly

参考 Apollo Cyber RT 设计的 **C++17 消息中间件**。公共 API 提供发布 / 订阅
（`Writer` / `Reader`，回调模式 + Observe 模式）与服务 / 客户端
（`Service` / `Client`，同步 + 异步请求）。消息类型必须是
`google::protobuf::Message` 派生类。

经过四批重构，传输与发现实现已与 Core 解耦：

- **Core 零 NNG**：四个 Core 只依赖 `Publisher` / `Subscriber` /
  `RpcRequester` / `RpcResponder` 四角色接口与 `IRegistry`，不含 `<nng/...>`。
- **默认 NNG 后端**：跨进程 Pub/Sub、RPC 与端点发现。
- **内存后端**：进程内共享总线，用于契约测试与无 NNG 构建验证（**非生产推荐**）。
- **构建可拆分**：`FIREFLY_WITH_NNG=OFF` 跳过 `find_package(nng)`，运行时自动
  组装内存后端。

## 目录结构

```
include/firefly/         公共头（仍以 "firefly/..." 路径包含）
  firefly.h init.h common/ message/ node/ service/
  detail/                公共模板所需句柄与工厂声明（桥接头）
src/
  core/                  四 Core、Node、注入契约（transport/registry/injection）
  backends/nng/          NNG transport、registry、net、默认注入
  backends/memory/       内存 transport、registry、默认注入
  runtime/               init.cc / runtime.cc（默认组装入口）
tests/
  contract/              双后端共享行为契约与故障注入
  integration/           公开 API 与跨进程 / 真实旧版互测
  support/               测试消息 proto
```

公共模板只包含句柄与工厂声明；传输角色与 Registry 的完整定义位于
`src/core/`。使用方只需 `#include "firefly/firefly.h"` 并链接 `firefly::firefly`，
无需感知内部目录划分。

## 一条命令验证

```bash
bash scripts/verify.sh
```

依次完成：默认 ON 配置 + 构建 + 全量 `ctest -j6`、4 组跨进程用例确认、
从零构造无 NNG 独立依赖前缀、OFF 配置 + 构建 + 全量测试。详见
[`docs/build.md` 的「如何验证」](docs/build.md#6-如何验证)。

## 构建选项

| 选项 | 默认 | 行为 |
| --- | --- | --- |
| `FIREFLY_WITH_NNG=ON` | ✅ | `find_package(nng CONFIG REQUIRED)`；`firefly::firefly` = Core + NNG 后端；默认注入 NNG；构建跨进程互测与 NNG-only 测试/示例 |
| `FIREFLY_WITH_NNG=OFF` | | **不调用** `find_package(nng)`；不编译、不链接任何 NNG 源文件与 target；默认注入内存后端；跳过 NNG-only 测试/辅助程序 |

两种开关下公共 API 签名、线协议（Pub/Sub 裸 protobuf、RPC FFRP 帧、发现文本协议）
与 target 拓扑均不变。`firefly_core` 始终自包含且从不链接 NNG。

```bash
# 默认 ON（vcpkg preset）
cmake --preset default && cmake --build --preset default && ctest --preset default

# 显式 OFF
cmake -S . -B build-off -DFIREFLY_WITH_NNG=OFF -DCMAKE_TOOLCHAIN_FILE=<vcpkg toolchain>
```

## 双后端组装说明

- 公共 API 不变。`firefly::Init()` 在运行时入口**一次**组装默认依赖
  （NNG 或内存），四个 Core 只调用 `detail::ResolveInjection()`，不自行选择后端。
- 测试或嵌入方需要替换时，通过 `InjectionContext` 注入
  `IEndpointFactory`（四角色工厂）与 `IRegistry`（记录模型）。
- `src/core/backend_access.h` 导出显式构造入口：
  `MakeMemoryEndpointFactory()` / `MakeMemoryRegistry()`（两种开关都可用）；
  `MakeNngEndpointFactory()` / `MakeNngRegistry()`（仅 ON）。共享契约测试
  `tests/contract/backend_contract_test.cpp` 用同一份源码分别跑内存与 NNG 两个实例。
- target 拓扑：`firefly_core_objects`（零 NNG）+ 后端「角色 / 注入」object +
  聚合 `firefly::firefly`。详见 `docs/build.md`。

## 内存后端定位

内存后端是**进程内共享总线**，满足四角色与 `IRegistry` 的全部行为契约
（取消、绝对截止时间、`Close()` 唤醒与幂等、响应对应、回调顺序、独立注册），
但它**不跨进程**，也不与 NNG 进程互通。

用途：

- 后端无关契约测试的基准实现；
- `FIREFLY_WITH_NNG=OFF` 的构建 / 链接 / 测试验证；
- 未安装 NNG 的环境下的 CI 与示例运行。

**非生产推荐**：多进程或跨主机部署请使用默认 NNG 后端（或按
`docs/new-backend-guide.md` 接入新的生产后端）。

## 跨进程测试前置

跨进程用例（`cross_*`，验证发现 / Pub/Sub / RPC 的旧版 raw-NNG 与新版公开 API
双向线兼容）需要：

- `FIREFLY_WITH_NNG=ON`，且 `firefly_interop_helper` 已构建；
- **Python 3**（仅标准库：`tests/integration/run_interop_pair.py` 用 `subprocess` /
  `threading`，无第三方依赖）；
- 4 组互不重叠的发现端口 `37501–37508`，由 CTest `ENVIRONMENT` 注入，可并行；
- `*_READY` 启动屏障：被期望成为发现主实例的进程先就绪，另一进程才启动。

已知偶发：高负载并行下 legacy 进程的 registry follower 可能遇到 3 秒 REQ
超时（`docs/acceptance-matrix.md` L10），`scripts/verify.sh` 仅对这类失败做
最多 3 次有限重试并留日志，其他失败立即报错。

## 依赖

使用 [vcpkg](https://github.com/microsoft/vcpkg) manifest 模式（本机位于
`D:\vcpkg`）：`protobuf` + `nng`。`FIREFLY_WITH_NNG=OFF` 时不需要 `nng`，
配置阶段也不会调用 `find_package(nng)`。

还需要：CMake ≥ 3.16、Ninja、Visual Studio 2022（含 C++ 桌面开发）、
Windows 10 SDK、Python 3。

## 构建与测试（手动）

```powershell
# 一键脚本（自动加载 MSVC vcvars64 环境）
powershell -ExecutionPolicy Bypass -File scripts/build.ps1
powershell -ExecutionPolicy Bypass -File scripts/build.ps1 -Action configure
powershell -ExecutionPolicy Bypass -File scripts/build.ps1 -Action test
```

或直接使用预设（`CMakePresets.json` 已把 toolchain 指向
`D:/vcpkg/scripts/buildsystems/vcpkg.cmake`，triplet `x64-windows`）：

```powershell
cmake --preset default
cmake --build --preset default
ctest --preset default
```

受限 shell 中 `vcvars64.bat` 可能不可用，此时用 `scripts/verify.sh`
（直接注入 MSVC `INCLUDE` / `LIB` / `PATH`，不调用 `vcvars64.bat`）。

## 网络 / 代理说明

protobuf 及 abseil 源码从 GitHub 下载。若网络无法直连 GitHub，请先启动代理：

```powershell
$env:HTTP_PROXY  = "http://<proxy-host>:<port>"
$env:HTTPS_PROXY = "http://<proxy-host>:<port>"
cmake --preset default
```

SOCKS5 代理使用 `socks5://host:port` 格式。

## 文档索引

| 文档 | 内容 |
| --- | --- |
| `docs/build.md` | 构建拆分契约、target 拓扑、测试分组、如何验证与逐项验收记录 |
| `docs/acceptance-matrix.md` | 四批 34 条验收标准 → 测试/构建证据/提交号的对照表与已知限制 |
| `docs/implementation.md` | 四批重构的原始规划稿（已标注完成状态） |
| `docs/implementation-notes.md` | 规划 vs 实际的落地差异（第 2/3 批落地说明） |
| `docs/new-backend-guide.md` | 接入新生产后端的最小实现面、契约测试集与「不碰 Core」边界 |
| `docs/release-notes.md` | 里程碑发布说明：逐批提交清单、验收结论、已知限制 |

## 状态

四批重构已完成：公共 API 签名、线协议与 target 拓扑保持不变；默认 ON 全量
30/30、无 NNG OFF 全量 24/24、并行 `ctest -j6` 稳定。已知限制见
`docs/acceptance-matrix.md` 第 5 节与 `docs/release-notes.md`。
