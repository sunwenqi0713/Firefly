# ADR-0006: 不引入 ZeroMQ 第三后端

- **状态**：Accepted（No-go 决策）
- **日期**：2026-11-24
- **决策者**：架构 owner、构建 owner
- **关联代码**：`src/core/transport.h`、`src/core/registry.h`、`src/backends/`、`vcpkg.json`
- **取代**：—

> 本 ADR 记录的是「**决定不做**」。重新评估的条件见 §4；未满足条件前不要重开评估。
> 完整可行性评估（成本分解、逐条技术风险、替代方案评分）已归档到团队 Wiki 的《Firefly ZeroMQ 后端可行性评估》页。

## 背景

提议为 Firefly 增加第三套生产后端 `zmq`（libzmq，可选 cppzmq）。动机有三类：客户现场只允许 libzmq；需要 CURVE 认证加密；或希望再验证一次「传输可替换」这个抽象。

评估结论是：**技术上成立，但架构收益接近零。**

- 缝是真的：`src/core/transport.h` 的四角色接口只有约 15 个虚函数，一个工厂接口，内存与 NNG 两套实现已跑同一份契约测试，Core 无需改动。
- 但第二后端已经把「传输可替换」证明完了，第三个后端不提供新的设计验证。
- 价值全部来自部署面。没有现场硬约束时，同等人力投到剩余缺口（RPC 兑现时限与发送裁决位置、`backend` 字段未用于选址、跨进程抖动）收益更高。

## 决策

**决定当前不引入 ZeroMQ 后端，也不引入 cppzmq 依赖。**

边界：

1. 「不做」是决策，不是待办。再次提出时先对照 §4 的重新评估条件，不满足即引用本 ADR 关闭。
2. `FIREFLY_WITH_NNG` 的默认语义与 `firefly::firefly` 的聚合方式不变；不新增 `FIREFLY_WITH_ZMQ`。
3. 传输可替换性已由内存后端与共享契约测试证明，不以「再加一个后端」作为验证手段。
4. 若未来重启，§5 的硬约束全部有效；其中 R3/R4（与 NNG 集群不互通）不可消除，只能文档化。

## 后果

- **正面**：省下 11–16 人日一次性投入，以及此后每个后端修复做两遍、验收矩阵翻倍的永久成本。
- **正面**：验收矩阵保持「内存 + NNG」两套，`verify.sh` 四条路径不变。
- **负面 / 成本**：只允许 libzmq 的现场无法直接部署 Firefly，需要桥接进程（评估 5–8 人日）作为替代路径。
- **负面 / 成本**：CURVE 加密需求在新后端路径上无解，只能在网络层或桥接层解决。
- **中性**：`backend` 字段保留在发现记录中，为将来多后端留位，但当前不用于选址（[known-gaps](../architecture/known-gaps.md) G2）。
- **如何验证仍被遵守**：`vcpkg.json` 无 `zeromq`/`cppzmq`；`grep -rn "zmq" src include CMakeLists.txt` 无匹配（架构文档中提及不算违规）。

## 4. 重新评估条件

满足**任一**才重开评估：

1. 出现明确的现场约束：客户环境只允许 libzmq，且不接受桥接进程。
2. 出现硬性加密要求（CURVE 或等价），且无法在网络层满足。
3. 需要 Firefly 直接对接既有 ZeroMQ 生态（第三方客户端，而非 Firefly 对 Firefly）。
4. 抽象层发生结构性变化（例如发现层抽出 `Channel` 接口），使新增后端成本降到 5 人日以内。

仅「想要第二后端」「验证抽象」「性能更好」不构成条件。

## 5. 若重启：硬约束（评估已确认）

| # | 约束 | 原因 |
| --- | --- | --- |
| R1 | 每个 socket 必须 `ZMQ_LINGER=0`；`zmq_ctx_t` 由工厂持有并引用计数，拆除顺序为「先关 socket → `zmq_ctx_shutdown` → `zmq_ctx_term`」 | 默认 `LINGER=-1` 会让 `zmq_close` 永久阻塞，直接违反「Close 返回后所有阻塞 I/O 已唤醒」的契约 |
| R2 | `io_mutex_` 包住 `zmq_poll(≤20ms)` + `zmq_msg_recv`，`Close()` 取同一把锁后再 `zmq_close` | socket 非线程安全，不允许「另一线程 poll 时 close」；20ms 切片与现有 NNG 行为等价 |
| R3 | 发现端口不得复用 NNG 的 37221/37222，改用独立端口；`FIREFLY_DISCOVERY_*_URL` 继续生效 | ZMQ 与 NNG 的 socket 无法对话；同端口会让两集群互抢 bind 后各自静默超时 |
| R4 | 一进程一后端，发现地址不得混配，写入硬约束文档 | Core 的 `Query` 不做 backend 过滤，混配会让 Reader 拿 ZMQ 地址去 NNG dial 并静默失败 |
| R5 | 只引 libzmq C API，自写约 60 行 RAII 包装；禁止 cppzmq 进入任何头文件 | 与现有 `NngSocket` 风格一致；避免异常风格与头文件泄漏面 |
| R6 | 发现层抽 `discovery::Channel`（Rep/Pub/Req/Sub 四个小接口），两后端各实现，**不复制** 848 行协议状态机 | 复制必然漂移；近期修复的两条 P1（旧客户端注册协议、Follower 初始快照）就落在这段代码里 |
| R7 | 新增 `FIREFLY_WITH_ZMQ`（默认 OFF）并与 NNG 配置期互斥；`verify.sh` 独立前缀需同时剥离 zeromq 及其 DLL | `FIREFLY_WITH_NNG` 默认语义是红线，不能改成三选一 |
| R8 | 里程碑闸门：S1（四角色）完成后用 `backend_contract_test zmq` 的 6 个 case 判定 go/no-go | 若 S1 卡在 R2 的关停竞态上，应立即停手 |

许可：libzmq 为 MPL-2.0（文件级 copyleft，静态链接不传染，需随二进制附许可声明）。

## 备选方案

| 方案 | 未采纳原因 |
| --- | --- |
| B. 直接做第三后端 | 11–16 人日 + 永久双维护；无现场约束时收益不成立 |
| C. 桥接进程（ZMQ ↔ Firefly 网关） | 5–8 人日，可独立部署；多一个进程与故障域，发现仍需单独解决。**现场受限时的首选替代** |
| D. 只做 ZMQ 传输、复用 NNG 发现 | 混编集群会让 NNG Reader 去 dial ZMQ 地址并静默失败；修法要动 Core（红线） |
| A. 不做，先清技术债 | **本决策采纳**：剩余缺口与跨进程抖动优先 |

## 修订记录

| 日期 | 变更 | 原因 |
| --- | --- | --- |
| 2026-11-24 | 初版（由可行性评估改写为决策记录） | 评估全文归档 Wiki；仓库只留决策与约束 |
