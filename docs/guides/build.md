# 构建与验证

依赖安装、CMake target 拓扑、开关语义与验证入口。行为约束见 [../architecture/contracts.md](../architecture/contracts.md)，测试细节见 [testing.md](testing.md)。

## 1. 前置

| 依赖 | 版本 / 说明 |
| --- | --- |
| 编译器 | 支持 C++17（MSVC 2022、GCC 9+、Clang 10+） |
| CMake | ≥ 3.21（`CMakePresets.json` 的 `cmakeMinimumRequired` 为 3.21） |
| Protobuf | 含 `protoc`。CMake 找不到时用 `-DPROTOC_EXE=/path/to/protoc` 指定 |
| NNG | 仅 `FIREFLY_WITH_NNG=ON` 时需要 |
| Python 3 | 仅跨进程测试需要，只用标准库 |
| vcpkg | 可选，用于自动获取 protobuf/nng（manifest 在 `vcpkg.json`） |

## 2. 常用命令

```bash
# 默认（NNG，Debug）
cmake --preset default
cmake --build --preset default
ctest --preset default

# 无 NNG（内存后端）
cmake -S . -B build-mem -G Ninja -DCMAKE_BUILD_TYPE=Debug -DFIREFLY_WITH_NNG=OFF
cmake --build build-mem --parallel
ctest --test-dir build-mem --output-on-failure
```

Windows 下若使用 vcpkg 工具链：

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release `
  "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## 3. target 拓扑

| target | 类型 | 内容 |
| --- | --- | --- |
| `firefly_core_objects` | OBJECT | 四 Core、`node.cc`、`endpoint_id.cc`、`log.cc`、`src/runtime/*.cc`。**不含** `<nng/...>`，不引用 NNG 符号 |
| `firefly_memory_role_objects` | OBJECT | `src/backends/memory/{transport,registry}.cc` |
| `firefly_memory_injection_objects` | OBJECT | `src/backends/memory/injection.cc` |
| `firefly_nng_role_objects` | OBJECT（仅 ON） | `src/backends/nng/{transport,registry}.cc` |
| `firefly_nng_injection_objects` | OBJECT（仅 ON） | `src/backends/nng/injection.cc` |
| `firefly_core` | STATIC | Core object + 内存角色 + 内存注入；**无 NNG**，可独立检查 |
| `firefly_nng` | STATIC（仅 ON） | NNG 角色 + NNG 注入 |
| `firefly` / `firefly::firefly` | STATIC | Core object + **一个**后端的角色与注入 object |

设计原因：注入符号每个后端各一份、最终链接只能出现一份；角色 object 需要被同一测试进程显式链接多次，以便共享契约测试同时构造内存与 NNG 实例。聚合库直接复用 object，避免同一静态库被归档两次导致符号冲突。

`configure_firefly_object(target)` 为 object 目标设置**私有** include 根（`include/`、`src/`）与编译选项；聚合库取哪套对象由 `FIREFLY_BACKEND_OBJECTS` 决定。

对外边界只有 `include/`：`firefly`、`firefly_core`、`firefly_nng` 的 `INTERFACE_INCLUDE_DIRECTORIES` 不含 `src/`，消费者无法包含 `core/*.h`、`backends/*`。需要内部头的测试与辅助程序显式链接 `firefly_internal`（`use_firefly_internal(<target>)`），这是测试特权。配置阶段有断言守着这条边界：把 `src/` 加回 PUBLIC 会直接配置失败。

## 4. 开关语义

| 开关 | 默认 | ON | OFF |
| --- | --- | --- | --- |
| `FIREFLY_WITH_NNG` | `ON` | `find_package(nng CONFIG REQUIRED)`，构建 `firefly_nng*`，默认注入 NNG | **不调用** `find_package(nng)`；默认注入内存后端；跳过直接依赖 NNG 的测试与示例 |
| `BUILD_TESTING` | `ON` | 生成测试、契约用例与两个示例（示例复用 `tests/support` 的 proto） | **不查找 `protoc`**，不生成 `test_messages`、测试与示例，只产出 `firefly` / `firefly_core` / `firefly_nng` 三个库 |

配置日志必须出现对应一行，这是排查「链接到了预期后端之外的东西」的第一判据：

- ON：`Firefly: NNG backend ENABLED`
- OFF：`Firefly: NNG backend DISABLED (in-memory backend selected)`

## 5. 后端注入入口

`src/core/interfaces.h` 声明、每个后端各实现一次，最终链接只出现一份：

| 函数 | 职责 |
| --- | --- |
| `ConfigureDiscovery(const DiscoveryConfig&)` | 首次 `Init` 前设置发现地址；空串保留当前值 |
| `BootstrapDefaultInjection()` | `Init()` 调用一次，组装默认工厂与 Registry |
| `ResetDefaultInjection()` | `Clear()` 调用，清空默认 ctx 并 `registry->Clear()` |
| `DefaultInjection()` | 懒组装兜底 |
| `ResolveInjection(ctx)` | 只补齐空字段，**不做后端选择** |
| `DefaultEndpointFactory()` / `DefaultRegistry()` | 返回本后端的工厂与注册中心 |

后端还导出 `Make<Nng|Memory>EndpointFactory()` / `Make<Nng|Memory>Registry()`，供 `src/core/backend_access.h` 的显式构造使用；测试据此在同一进程内构造两个后端，不依赖 `Default*` 的选择。

## 6. 测试分组

| 分组 | 何时构建 | 内容 |
| --- | --- | --- |
| `core-tests` | 两种开关 | 不直接依赖 NNG 的测试；OFF 时默认注入为内存 |
| `nng-tests` | 仅 ON | `discovery_test`、`interop_helper`、共享契约的 NNG 实例 |
| `shared-backend-contract` | 内存实例始终；NNG 实例仅 ON | 同一份 `tests/contract/backend_contract_test.cpp` 两实例 |
| `cross-process` | 仅 ON | 7 个两进程 CTest，端口对 37501–37514，`*_READY` 启动屏障，计数由 `scripts/verify.sh` 从 CTest 注册表读取 |

`-DBUILD_TESTING=OFF` 时以上分组全部不生成，`ctest` 显示 `Total Tests: 0`。

细节与运行方式见 [testing.md](testing.md)。

## 7. 一条命令验收

```bash
bash scripts/verify.sh
```

脚本在干净检出上依次跑四条路径，任一阶段失败即非 0：

1. **默认 ON**：vcpkg 配置（日志 `NNG backend ENABLED`）+ 构建 + 全量 `ctest -j6`。
2. **跨进程确认**：从全量日志核对 4/4 `cross_*` 通过，不重复启动进程。
3. **无 NNG 独立前缀**：从 `vcpkg_installed` 只复制非 NNG 内容到独立前缀（不删除任何东西），断言前缀中无 `*nng*` 头/库/包配置。
4. **OFF（内存）**：不使用 vcpkg 工具链，配置日志确认 `NNG backend DISABLED`，构建 + 全量 `ctest`。

日志位置：`build/configure-on.log`、`build/ctest-on.log`、`build-no-nng/configure-off.log`。

| 变量 / 选项 | 默认 | 用途 |
| --- | --- | --- |
| `VCPKG_ROOT` | `D:/vcpkg` | vcpkg toolchain 与 ninja 位置 |
| `VCPKG_TRIPLET` | `x64-windows` | 安装的 triplet |
| `MSVC_TOOLS` | 本机版本 | `VC/Tools/MSVC/<版本>` 目录 |
| `WINSDK_ROOT` / `WINSDK_VER` | 本机版本 | Windows SDK 目录与版本 |
| `NINJA` | vcpkg 自带 ninja | `CMAKE_MAKE_PROGRAM` |
| `JOBS` | `6` | 构建与 `ctest` 并行度 |
| `FIREFLY_SKIP_MSVC_ENV=1` | 未设置 | 已在 VS 开发环境中时跳过环境注入 |
| `FIREFLY_DEPS_DIR` | `build-no-nng/deps` | 在全新目录验证「从零构造前缀」 |
| `--skip-on` / `--skip-off` / `--skip-interop` | 未设置 | 单跑某条路径 |

脚本直接注入 MSVC `INCLUDE` / `LIB` / `PATH`，不调用 `vcvars64.bat`（受限 shell 中该脚本会触发 `reg.exe` 拦截）。`CMakePresets.json` 与 `scripts/build.ps1` 仍可用于日常开发；`verify.sh` 是**验收入口**。

## 8. 构建自检

改动构建或公共头后，逐条确认：

- [ ] 默认 `firefly::firefly` 存在，内部为 Core object + 一个后端；对外链接接口未变。
- [ ] `firefly_core` 不链接 NNG、`no_leak_check` 通过。
- [ ] OFF 跳过 `find_package(nng)`，配置日志为 `DISABLED`。
- [ ] 无 NNG 环境完成配置、构建、链接与测试（独立前缀中无 `*nng*`）。
- [ ] 同一套上层调用在 NNG / 内存两种组装下通过同一套契约测试。
- [ ] `ctest -j6` 连续两轮稳定；两个示例退出码 0。
- [ ] 公共 API 签名、FFRP 帧、发现文本协议、payload 格式均未变。
- [ ] `-DBUILD_TESTING=OFF` 的配置不查找 `protoc`，`ctest -N` 为 0 个用例。
- [ ] `cmake --install` 产出头、三个库与包配置，独立工程 `find_package(firefly CONFIG)` 能构建并运行。

测试抖动与环境限制见 [../architecture/known-gaps.md](../architecture/known-gaps.md) §三。

## 9. API 文档生成

```bash
doxygen Doxyfile          # 输出到 docs/api/，不入版本控制
```

`Doxyfile` 只扫描 `include/firefly`，`WARN_AS_ERROR=YES`：文档告警即失败。注释规范见 [documentation.md](documentation.md) 第三步。门禁镜像固定在 Ubuntu 24.04（Doxygen 1.9.x 基线），版本漂移会先于文档生成失败。

## 10. 集成与安装

两种接入方式共用同一份消费者代码，target 名一致：

| 方式 | 用法 | 说明 |
| --- | --- | --- |
| `add_subdirectory` | `set(BUILD_TESTING OFF)` 后 `add_subdirectory(Firefly)`，链接 `firefly::firefly` | 不要求 `protoc`；`firefly_internal` 只对测试可见 |
| `find_package` | `cmake --install build --prefix <prefix>`，再 `find_package(firefly CONFIG REQUIRED)` | 安装头、`lib/firefly{,_core,_nng}.lib` 与 `lib/cmake/firefly/` 包配置 |

```cmake
find_package(firefly CONFIG REQUIRED)
target_link_libraries(app PRIVATE firefly::firefly)   # 默认后端
# target_link_libraries(app PRIVATE firefly::core)    # 只含内存后端，从不依赖 NNG
```

包配置会 `find_dependency` 出 `protobuf`、`Threads`，ON 时还有 `nng`：消费方需自备这些依赖的 CMake 包（vcpkg 工具链即可）。

**不要同时链接 `firefly::firefly` 与 `firefly::core`**：两者都包含 Core object，会重复定义符号；同理不要同时链接 `firefly::firefly` 与 `firefly::nng`。
