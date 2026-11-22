# Firefly 构建拆分契约（第 4 批）

本文件记录第 4 批「用构建和契约测试证明可替换」的构建契约、target 拓扑、
开关语义、测试分组和验收记录。本批不改 Core 行为语义、不改线协议、不改公共
API 签名、不引入新第三方依赖。

## 1. 通读后的现状清单

### 1.1 当前 target 与源文件归属

- 唯一静态库 `firefly`（`CMakeLists.txt`）：聚合入口 `init.cc`、运行时
  `runtime.cc`、日志 `common/log.cc`、发现层 `detail/discovery.cc`、
  `detail/endpoint_id.cc`、四 Core、NNG 适配器 `detail/transport_nng.cc` 与
  `node/node.cc`；`PUBLIC` 链接 protobuf + nng + Threads。
- 测试可执行文件全部在根 `CMakeLists.txt` 注册（无 `tests/CMakeLists.txt`），
  统一 `PRIVATE firefly`，多数另加 `test_messages`。示例 2 个、`interop_helper`
  1 个也链接同一聚合库。

### 1.2 测试/示例对 NNG 的依赖

- 直接源码依赖：`tests/batch3_contract_test.cpp`、
  `tests/interop_helper.cpp`（`<nng/...>` 与 `detail/net.h` 的
  `ListenOnEphemeralPort`）；`tests/discovery_test.cpp` 直接使用
  `firefly::discovery::Registry`，强绑定 NNG 发现实现。
- 间接依赖：其余测试与示例都经 `firefly` 传递链接 `nng::nng`；
  `fake_transport_test.cpp` 还调用 `DefaultEndpointFactory()` 跑真实 NNG 适配器；
  所有使用默认 `Init`/`CreateNode` 的测试都经默认注入走到 NNG。
- 纯 API/编译校验测试（`header_check`、`error_code`、`message_traits` 等）
  自身不调用传输，但仍因链接聚合库而带入 NNG。

### 1.3 Core 源文件清单（必须留在无 NNG target）

- 入口/运行时：`firefly/init.cc`、`firefly/runtime.cc`、`firefly/common/log.cc`
- 类型与句柄：`firefly/detail/endpoint_id.cc`、`firefly/detail/handles.h`、
  `firefly/detail/rpc_frame.h`、`firefly/common/types.h`
- 四 Core：`firefly/detail/writer_core.cc`、`reader_core.cc`、
  `service_core.cc`、`client_core.cc`
- Node 与公共头：`firefly/node/*`、`firefly/service/*`、`firefly/firefly.h`、
  `firefly/detail/interfaces.h`、`firefly/detail/endpoint_api.h`

NNG 实现源文件：`firefly/detail/transport_nng.cc`、
`firefly/detail/discovery.cc`、`firefly/detail/net.h`，全部归入 NNG target。

### 1.4 内存实现现有缺口

生产代码没有内存后端；只有 `tests/fake_transport_test.cpp` 内的测试 Fake。
对照角色/注册表契约，缺失：

- 四角色：`Publisher::Bind/Send/Close`、`Subscriber::UpdatePublishers/Receive/Close`、
  `RpcRequester::Exchange/Close`、`RpcResponder::Bind/Receive/Reply/Close` 的可复用实现。
- `IRegistry`：`EndpointRecord` 记录模型、按 id 注销、同键多记录、backend 过滤、
  `Watch` 初始/后续完整快照、`Unwatch`、`Clear` 后迟到注销 no-op。
- 默认组装：`DefaultEndpointFactory/DefaultRegistry/BootstrapDefaultInjection/
  ResetDefaultInjection/ResolveInjection/ConfigureDiscovery` 目前都在
  `transport_nng.cc`，OFF 构建会缺符号。

### 1.5 运行时组装点

唯一入口 `firefly::Init()` → `detail::BootstrapDefaultInjection()`
（`firefly/init.cc:16`）；默认组装逻辑在 `firefly/detail/transport_nng.cc`
尾部。`Clear()` 调 `ResetDefaultInjection()`；`ResolveInjection()` 只补空字段。

## 2. 构建拆分契约

### 2.1 target 拓扑

- `firefly_core_objects`（OBJECT）：1.3 的 Core 源文件，不含任何 `<nng/...>`、
  不引用任何 NNG 符号。
- `firefly_memory_role_objects` / `firefly_memory_injection_objects`（OBJECT）：
  内存四角色与注册表 / 默认注入；只依赖接口头。角色 object 可单独链接，
  使同一测试进程能同时构造内存与 NNG 后端而不重复注入符号。
- `firefly_nng_role_objects` / `firefly_nng_injection_objects`（OBJECT，仅 ON）：
  `transport_nng.cc`/`discovery.cc` 与 `nng_injection.cc`；链接 `nng::nng`。
- `firefly_core`（STATIC）：Core object + 内存角色 object + 内存注入 object；
  自包含，无 NNG。
- `firefly_nng`（STATIC，仅 ON）：NNG 角色 object + NNG 注入 object。
- `firefly::firefly`（聚合静态库）：默认由 Core object + NNG 后端 object 组装；
  ON 时对外接口与阶段三完全一致。OFF 时由 Core object + 内存后端 object 组装，
  等价于内存 core。

说明：聚合库直接复用 Core object 与后端 object，避免同一静态库被归档两次导致
符号冲突；后端"角色/注入"再拆一层是为了共享契约测试能显式构造两个后端。
`firefly_core` 与 `firefly_nng` 仍作为可独立检查的 CMake target 存在。

### 2.2 开关

- `option(FIREFLY_WITH_NNG "Build NNG transport/registry backend" ON)`
- ON：`find_package(nng CONFIG REQUIRED)`，构建 `firefly_nng*`，默认注入 NNG。
- OFF：**不调用** `find_package(nng)`，不编译/不链接任何 NNG 源文件与 target；
  默认注入内存后端；跳过所有直接依赖 NNG 的测试/示例；配置日志打印
  `Firefly: NNG backend DISABLED (in-memory backend selected)`。

### 2.3 后端注入接口（detail 层）

为了保持 `ResolveInjection` 与公共 API 不变，后端提供同一组实现：

- `BootstrapDefaultInjection()` / `ResetDefaultInjection()` /
  `DefaultEndpointFactory()` / `DefaultRegistry()` / `DefaultInjection()` /
  `ConfigureDiscovery()` 在每个后端各实现一次；最终链接只出现一份。
- 内存后端额外导出 `MakeMemoryEndpointFactory()`、`MakeMemoryRegistry()`，
  NNG 后端额外导出 `MakeNngEndpointFactory()`、`MakeNngRegistry()`，供
  `detail/backend_access.h` 的后端无关测试显式构造；测试不依赖 `Default*` 的后端选择。

### 2.4 测试分组

- `core-tests`（两种开关都构建）：不直接依赖 NNG 的测试；OFF 时默认注入为内存。
- `nng-tests`（仅 ON）：`discovery_test`、`interop_helper` 及共享契约测试的 NNG 实例。
  `batch3_contract_test` 两种开关都构建，NNG 部分用 `FIREFLY_WITH_NNG` 条件编译。
- `shared-backend-contract`：同一测试源两份实例，内存实例始终构建，NNG 实例仅 ON；
  覆盖取消、绝对截止时间、`Close()` 跨线程唤醒与幂等、响应对应、回调顺序、独立注册；
  NNG 实例设置独立发现地址（37401/37402）并加 `RESOURCE_LOCK firefly_registry_port`。
- `cross-process`（仅 ON）：4 个两进程 CTest，验证发现、Pub/Sub、RPC 两个方向；
  端口对 37501/37502、37503/37504、37505/37506、37507/37508 互不重叠，
  不加共享锁，依赖 `*_READY` 标记作为跨进程启动屏障。

## 3. 行为不变量

- 公共头、公共 API 签名、端点模板壳不变；Core 不 include `<nng/...>`。
- 线协议（Pub/Sub 裸 protobuf、RPC FFRP、发现 REG/UNREG/QLIST 文本）不变。
- 第 1/2/3 批全部测试与示例在默认 ON 构建下保持通过。

## 4. 验收记录

### T1（默认 ON 拆分，提交 `d3c753b`）

- `cmake --preset default` 日志：`Firefly: NNG backend ENABLED`。
- 全量默认构建成功；`ctest --test-dir build`：24/24 通过。
- `firefly_pubsub_example`、`firefly_rpc_example` 退出码均为 0。
- `firefly_core` 由 Core object + 内存后端 object 组成，链接接口只有 protobuf 与 Threads；
  Core object 源文件无 `<nng/...>`、无 `NNG_*` 符号。

### T2（内存后端补全，提交 `b7b8c21`）

- 内存后端以 `MakeMemoryEndpointFactory()`/`MakeMemoryRegistry()` 实现四角色与记录模型；
  `batch3_contract_test` 的 NNG 端口选择改为 `FIREFLY_WITH_NNG` 条件编译。
- `build-mem`（`FIREFLY_WITH_NNG=OFF`，仍带 vcpkg 工具链）全量构建成功，23/23 通过。
- 默认 ON 下重跑 `batch1_contract_test`、`batch3_contract_test`：2/2 通过。

### T3（真正无 NNG 环境，提交 `46003e9`）

- 独立依赖前缀 `build-no-nng/deps`：只保留 protobuf/abseil/utf8-range 的
  share/lib/bin/include/tools；删除 `share/nng`、`include/nng` 及所有 `*nng*` 库/DLL。
  配置**未使用 vcpkg 工具链**，因此 manifest 不会安装 nng。
- 配置命令额外传 `-DCMAKE_DISABLE_FIND_PACKAGE_nng=TRUE` 作为兜底；CMakeLists 的 OFF
  分支本身不调用 `find_package(nng)`。日志确认：
  `Firefly: NNG backend DISABLED (in-memory backend selected)`。
- `cmake --build build-no-nng`：65/65 步完成，无 NNG 编译单元、无 NNG 链接输入。
- `ctest --test-dir build-no-nng --output-on-failure`：23/23 通过。

### T4（共享后端契约，提交 `8f1e8b7`）

- `tests/backend_contract_test.cpp` 一份源码两种实例：
  `backend_contract_memory` 与 `backend_contract_nng`。
- 覆盖排队中取消（服务端 0 次执行）、排队请求绝对截止时间、`Close()` 跨线程唤醒
  与幂等（Subscriber/Responder）、响应对应、回调顺序、同键独立注册。
- 默认 ON：`backend_contract_memory` 与 `backend_contract_nng` 均通过，
  全量 26/26；无 NNG 前缀：仅内存实例，全量 24/24。

### T5/T6（跨进程与文档收尾，提交见本步）

- `tests/run_interop_pair.py` 用标准库启动两进程，`*_READY` 标记作为启动屏障；
  4 组用例端口对 37501–37508 互不重叠，独立发现地址，无需共享锁。
- 跨进程覆盖 legacy_pub→new_sub、new_pub→legacy_sub、legacy_rep→new_req、
  new_rep→legacy_req，验证发现、Pub/Sub、RPC 双向线兼容，4/4 通过。
- 默认全量并行 `ctest -j6` 连续两轮 30/30 通过；两个示例退出码 0。
- 公共 API 签名、FFRP 帧、发现文本协议与 protobuf 载荷格式均未改动。

## 5. 自检清单

- [x] 默认 `firefly::firefly` target 存在，内部拆为 Core object + NNG backend。
- [x] Core target 不链接 NNG、不 include `<nng/...>`。
- [x] OFF 跳过 `find_package(nng)`，编译排除 NNG target。
- [x] 无 NNG 环境完成配置、构建、链接、测试。
- [x] 同一套上层调用在 NNG/内存组装下通过同一套行为契约。
- [x] 共享契约覆盖取消、截止时间、`Close()` 唤醒/幂等、响应对应、回调顺序、独立注册。
- [x] 跨进程覆盖发现、Pub/Sub、RPC，独立地址，并行 CTest 稳定。
- [x] 前三批测试与示例仍通过；公共 API 签名、线协议不变。
