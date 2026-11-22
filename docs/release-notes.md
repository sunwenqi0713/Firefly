# Firefly 里程碑发布说明：`refactor-complete`

- **里程碑 tag**：`refactor-complete`
- **日期**：2026-11-22
- **分支策略**：单主干 `main`。推送前 `origin/main` 为 `46b6d0e`，本地在其上
  线性推进 22 个提交，本次为 **fast-forward 推送**，无 rebase、无 force push。
- **范围**：四批重构（Core 零 NNG、四角色接口、发现层可替换、运行时集中组装、
  构建可拆分、无 NNG 环境可构建、跨进程互测）+ 第 5 批收尾（仓库卫生、CI
  固化、文档收口、推送打标、新后端接入指南）。

## 一、里程碑结论

Firefly 的通信语义（Core）已与传输/发现实现完全解耦：

- 四个 Core 只依赖 `Publisher` / `Subscriber` / `RpcRequester` / `RpcResponder`
  四角色接口与 `IRegistry` 记录模型，源码中不含 `<nng/...>` 或 NNG 错误码。
- 默认 `firefly::firefly` 仍聚合 NNG 后端，对外接口与线协议完全不变。
- `FIREFLY_WITH_NNG=OFF` 时不调用 `find_package(nng)`，运行时组装内存后端，
  在未安装 NNG 的独立依赖前缀下完成配置、构建、链接与测试。
- 同一套上层调用在 NNG 与内存两种组装下通过**同一份**行为契约测试。

**验收对照表结论**：四批共 **34 条验收标准全部「已覆盖」**，逐条映射到
CTest 用例 / 构建日志 / 提交号，无遗留验收缺口；另记录 **11 条已知限制**，
均给出边界与结论。完整表格见 `docs/acceptance-matrix.md`。

| 验收项 | 结果 |
| --- | --- |
| 默认 ON 全量 `ctest -j6` | 30/30 通过（含共享契约双实例、4 组跨进程用例） |
| 无 NNG OFF 全量 `ctest` | 24/24 通过，配置日志 `NNG backend DISABLED` |
| 并行稳定性 | `ctest -j6` 连续两轮 30/30 |
| 跨进程互测 | legacy/new 双向 Pub/Sub 与 RPC，4 组独立地址全绿 |
| 两个示例 | `firefly_pubsub_example`、`firefly_rpc_example` 退出码 0 |
| 兼容性 | 公共 API 签名、FFRP 帧、发现文本协议、protobuf 载荷、target 拓扑均未变 |

一条命令复现全部验收：`bash scripts/verify.sh`（详见 `docs/build.md` 第 6 节）。

## 二、逐批提交清单

> 口径：`git log --oneline 46b6d0e..main` 共 **22 个提交**，按第 1–5 批归类。
> 每个批次给出「代表提交 + 子提交列表 + 验收结论」三级映射；完整历史未被
> rebase 或 squash，本表只提供文档导航。基线之前的提交作为背景保留如下。

| 基线/阶段 | 提交 | 内容 |
| --- | --- | --- |
| 基线 | `f07e4c6` | 初始提交 |
| 阶段一 | `41e5491` | 显式化通信语义与错误模型（`ErrorCode` / `Result<T>` / `WriteStatus`） |
| 阶段二 | `218e10b` | 明确生命周期、并发与取消契约 |
| 阶段三 | `df3c1da` | 剥离传输细节，补齐可观测性、注入点与上下文回调 |
| 审查修复 | `46b6d0e` | 修复审查发现的生命周期与 RPC 竞态问题（推送前远端基线） |

### 第 1 批：修正端点生命周期和 RPC 行为

- **代表提交**：`69015bb` — 修正 Service 释放停机与回调线程内停机竞态
- **子提交**：`69015bb`（Service 释放停机）、`166da9d`（DeleteReader 删除中名称占用与并发 GetReader）、`682e2f2`（RPC 统一有界队列、绝对截止时间预算与两态取消）
- **验收结论**：见 `docs/acceptance-matrix.md` 第 1 节（1.1–1.8），CTest 全部通过。

### 第 2 批：按角色迁移 NNG 数据传输

- **代表提交**：`c379dc0` — 发布订阅迁移到四角色接口
- **子提交**：`c379dc0`（Pub/Sub 角色迁移）、`32e4dad`（RPC 迁移到角色接口并保留两态取消）、`30dfb0b`（收口 NNG 角色适配器、移除过渡桩）、`ee4b006`（删除 `ISocket` 并完成新旧进程互测）
- **验收结论**：见 `docs/acceptance-matrix.md` 第 2 节（2.1–2.9），CTest 全部通过。

### 第 3 批：拆开端点发现与默认组装

- **代表提交**：`3d26888` — Registry 记录模型与 `Watch` 快照
- **子提交**：`3d26888`（Registry 记录模型与 Watch 快照）、`11a2b33`（四 Core 接入记录模型与地址集合更新）、`cb0751a`（运行时入口一次组装默认依赖）、`34f0bdc`（记录启动销毁与迟到注销契约）、`af2665f`（第 3 批发现层契约测试与文档）
- **验收结论**：见 `docs/acceptance-matrix.md` 第 3 节（3.1–3.9），CTest 全部通过。

### 第 4 批：用构建和契约测试证明可替换

- **代表提交**：`d3c753b` — 拆 Core / NNG / 聚合 target 并保持默认全绿
- **子提交**：`d3c753b`（target 拆分）、`b7b8c21`（内存后端通过批 1/批 3 契约并支持 OFF 测试分组）、`46003e9`（无 NNG 环境完成 OFF 构建与全量测试）、`8f1e8b7`（同一后端无关契约测试覆盖内存与 NNG）、`7664b2c`（跨进程发现/PubSub/RPC 互测与文档验收收尾）
- **验收结论**：见 `docs/acceptance-matrix.md` 第 4 节（4.1–4.8），CTest 全部通过。

### 第 5 批：收尾与交接

- **代表提交**：`ddee6fa` — 仓库卫生：忽略验证目录、规范隐藏字符文档名、纳入验收对照表
- **子提交**：`ddee6fa`（仓库卫生）、`5c9f1fe`（`scripts/verify.sh` 双路径验收）、`ef8de07`（README 与 `docs/build.md` 文档收口）、`7132414`（发布说明、推送与里程碑 tag）、`b8f8e18`（新后端接入指南与发布说明提交号收口）
- **验收结论**：见第 5 批收尾说明与 `docs/build.md` 第 6 节，CTest 全部通过。

完整清单以 `git log --oneline 46b6d0e..main` 为准；tag `refactor-complete`
指向 `7132414`，`b8f8e18` 为其后的文档收尾提交，两者均已在 `main` 上。

## 三、主要产出文件

- 接口契约：`firefly/detail/interfaces.h`、`firefly/detail/endpoint_api.h`、
  `firefly/detail/backend_access.h`
- 内存后端：`memory_transport.cc`、`memory_registry.cc`、`memory_injection.cc`
- NNG 后端：`transport_nng.cc`、`discovery.cc`、`nng_injection.cc`
- 构建：`CMakeLists.txt`（`FIREFLY_WITH_NNG`）、`CMakePresets.json`
- 测试：`tests/backend_contract_test.cpp`、`batch1_contract_test.cpp`、
  `batch3_contract_test.cpp`、`interop_helper.cpp`、`run_interop_pair.py`、
  `tools/check_no_leak.py`
- CI/文档：`scripts/verify.sh`、`docs/build.md`、
  `docs/acceptance-matrix.md`、`docs/new-backend-guide.md`、本文件

## 四、已知限制（摘要）

完整 11 条见 `docs/acceptance-matrix.md` 第 5 节，重点：

1. **内存后端不跨进程**，定位为契约测试与无 NNG 构建验证，非生产推荐。
2. **新生产后端与 NNG 进程直接互通**未做，需要共同线协议或桥接。
3. **全局 `Shutdown()` 只置标志**，不级联关闭端点；端点须单独停机/析构。
4. **QoS 协商 / 背压与 mps 限流 / 跨进程同名冲突 / 请求幂等去重 /
   Registry 订阅回调持锁派发 / `RuntimeConfig` 外部配置 / 运行时 schema
   版本协商** 均为 out-of-scope TODO。
5. **二进制 ABI 不兼容**：源码级 `[[deprecated]]` 兼容已做，但返回类型变更
   导致 ABI 不兼容，真正的二进制灰度需版本化 soname。
6. **跨进程用例偶发**：并行高负载下 legacy follower 可能 3 秒 REQ 超时
   （L10），`verify.sh` 仅对此做最多 3 次有限重试；**OFF 构建目录反复重配**
   会混用链接产物（L11），脚本通过前缀绑定构建目录规避。二者均为测试环境
   时序问题，非产品缺陷，未改动 Core / 线协议 / 公共 API / target 拓扑。

## 五、兼容性声明

- 公共 API 签名、`Writer` / `Reader` / `Service` / `Client` 行为语义未变。
- 线协议未变：Pub/Sub 裸 protobuf 载荷、RPC `FFRP` 响应帧、发现
  `REG` / `UNREG` / `QLIST` 文本协议（仅追加可选 id/backend 字段，旧版忽略）。
- `firefly::firefly` 聚合 target 拓扑与对外链接接口未变。
- 构建新增可选开关 `FIREFLY_WITH_NNG`，默认 ON，不改变既有构建方式。

## 六、压缩历史与原始历史的对应关系（`refactor-compact` 分支）

本分支 `refactor-compact`（里程碑 tag `refactor-complete-v2`）把
`46b6d0e..main` 的 **24 个提交**按批次压缩为 **5 个提交**，便于浏览；
它**不替换**原始历史，也不改写 `main` 与旧 tag。

| 压缩后提交（`refactor-compact`） | 批次 | 覆盖的原始子提交（`main` / `refactor-complete`） |
| --- | --- | --- |
| `df5efca` | 第 1 批 | `69015bb`、`166da9d`、`682e2f2` |
| `d8d9183` | 第 2 批 | `c379dc0`、`32e4dad`、`30dfb0b`、`ee4b006` |
| `8d57681` | 第 3 批 | `3d26888`、`11a2b33`、`cb0751a`、`34f0bdc`、`af2665f` |
| `cb3602f` | 第 4 批 | `d3c753b`、`b7b8c21`、`46003e9`、`8f1e8b7`、`7664b2c` |
| 本提交（HEAD） | 第 5 批 | `ddee6fa`、`5c9f1fe`、`ef8de07`、`7132414`、`b8f8e18`、`e536c52`、`a09846b` |

**权威性说明**：

- `main` 与 tag `refactor-complete`（指向 `7132414`）是**权威历史**：保留全部
  逐步提交、`git bisect` 粒度，以及 `docs/acceptance-matrix.md` /
  `docs/build.md` 中引用的 hash。
- 本分支与新 tag 仅提供**展示用的压缩视角**；除本节新增的双历史映射外，
  末提交树与 `main`（`a09846b`）完全一致，代码、测试、CMake、公共 API、
  线协议均未改变。
- 后续若需在这两套历史上同时修复问题，应先落到 `main`，再把对应变更
  cherry-pick 到 `refactor-compact`；不要在压缩分支上独立分叉产品语义。
