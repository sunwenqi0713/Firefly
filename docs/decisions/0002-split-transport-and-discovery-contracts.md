# ADR-0002: 传输与发现拆为两个可替换契约

- **状态**：Accepted
- **日期**：2026-11-24
- **决策者**：架构 owner、Core owner
- **关联代码**：`include/firefly/detail/endpoint_api.h`、`src/core/interfaces.h`、`src/core/transport.h`、`src/core/registry.h`
- **取代**：—

## 背景

改造前，Core 虽然通过一层 `ISocket` 挡住了公共头里的传输类型，但依赖并未真正切断：`WriterCore` 直接判断 `NNG_EAGAIN`，默认 `Registry` 直接用 NNG socket，Core 仍需理解 `kPub/kSub/kReq/kRep`、`Bind/Dial`、socket 超时与整数错误码。

结果是「换后端」实际上要改 Core：传输细节与发现机制纠缠在同一个抽象里，替换成本等于重写。

## 决策

**决定把替换点下移到 `Reader`/`Writer`/`Client`/`Service` 的 Core 之下，并把「数据传输」与「端点发现」拆成两个彼此独立、可分别替换的契约。**

边界：

1. 公共 API 不变：仍用 Protobuf 类型表达消息，`CreateXxx` / `Write` / `SendRequest` / 回调签名保持。
2. Core 只负责 Firefly 语义（排队、取消、兑现、缓冲、回调和关闭），**只处理字节**，不做序列化之外的消息解释。
3. 传输负责监听、连接、按消息收发、解除阻塞，并把后端错误转换为 `TransferStatus`。
4. 发现负责注册、注销、查询与变更订阅；地址由后端解释，Core 不解析。
5. 两者分别注入：同一发现实现可配不同传输，反之亦然。

## 后果

- **正面**：Core 与后端解耦；内存后端证明可用同一套 Core 代码在进程内跑通全部契约；无 NNG 构建成为可能。
- **正面**：发现层可独立演进，线协议兼容问题收敛在发现实现内。
- **负面 / 成本**：一次跨 Core 边界的重构，四个 Core 与全部测试都要改；契约测试必须能同时跑两套后端，否则「可替换」无法证明。
- **中性**：新增后端从「改 Core」变成「加实现 + 过契约测试」，代价转移给后端作者。
- **如何验证仍被遵守**：`no_leak_check`（Core 无 NNG 符号/头文件）、`backend_contract_memory` 与 `backend_contract_nng` 双实例、`FIREFLY_WITH_NNG=OFF` 全量构建与测试。

## 备选方案

| 方案 | 未采纳原因 |
| --- | --- |
| 保留单一 `ISocket` 抽象，只把 NNG 调用包装起来 | 发现层的 848 行协议状态机仍绑在 NNG socket 上，换后端仍要改 Core；已证明不可替换 |
| 只在编译期用宏切换后端 | 同一进程无法构造两个后端，共享契约测试做不成，替换能力无法被验证 |
| 抽象更粗的「通信中间件」接口 | 会把发布订阅与请求应答的语义差异塞进一个接口，Core 无法表达四类需求 |

## 修订记录

| 日期 | 变更 | 原因 |
| --- | --- | --- |
| 2026-11-24 | 初版（从重构规划稿抽取） | 规划稿移出仓库，决策单独留存 |
