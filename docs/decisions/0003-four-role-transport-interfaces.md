# ADR-0003: 传输按四角色建模，取代通用 ISocket

- **状态**：Accepted
- **日期**：2026-11-24
- **决策者**：架构 owner、Core owner
- **关联代码**：`src/core/interfaces.h`、`include/firefly/detail/endpoint_api.h`、`CMakeLists.txt`
- **取代**：ADR-0002 中的 `ISocket` 部分

## 背景

拆除 NNG 依赖后仍有一个遗留问题：一个通用 socket 接口同时服务发布、订阅、请求、应答四种用途，导致接口包含大量「某个角色才需要」的操作（`Bind` 与 `Dial` 并存、超时既可设又可按次传入）。适配器只能用注释说明「哪些方法在哪个角色下有效」，Core 也无法在类型上表达自己的需求。

## 决策

**决定用四个角色接口替代通用 `ISocket`：`Publisher`、`Subscriber`、`RpcRequester`、`RpcResponder`，每个接口只声明该角色必须的操作。**

边界：

1. 角色接口由 `IEndpointFactory` 创建，`Core` 只持有角色指针。
2. 四个角色的 `Close()` 都幂等且可跨线程调用，返回后阻塞 I/O 已唤醒并以 `kClosed` 结束。
3. `RpcRequester::Exchange` 接收**绝对 deadline**，在连接/发送/接收每个阻塞点前重算剩余预算。
4. `RpcResponder` 用**不透明** `ReplyToken` 关联请求与响应，Core 只原样回传。
5. 后端专有错误码不得跨过适配器边界，统一收敛到 `TransferStatus`。
6. 后端拆成「角色 object」与「注入 object」两类 OBJECT target：角色 object 可被同一测试进程多次链接，注入 object 每后端一份、聚合库只选其一。

## 后果

- **正面**：接口面收窄，契约可按角色逐条测试；适配器无法「用不到的方法先留空实现」。
- **正面**：同一份契约测试可在内存与 NNG 两个后端上运行（`backend_contract_test` 双实例）。
- **负面 / 成本**：每个角色各一套实现，样板代码比单一 socket 多约 15 个虚函数；新增后端要写四个类而不是一个。
- **负面 / 成本**：target 拓扑变复杂（5 个 OBJECT + 3 个 STATIC），构建脚本与文档必须同步维护。
- **如何验证仍被遵守**：`backend_contract_test` 的 6 个 case（取消、绝对截止时间、`Close()` 唤醒与幂等、响应对应、回调顺序、独立注册）；`fake_transport_test` 验证状态映射；`firefly_core` 链接接口不含 NNG。

## 备选方案

| 方案 | 未采纳原因 |
| --- | --- |
| 单一接口 + 运行时 `dynamic_cast` 校验角色 | 契约从类型系统退化为运行时断言，错误推迟到调用时 |
| 四个角色共用一个基类并保留全部方法 | 等于没拆，适配器仍要面对用不上的操作 |
| 用模板策略注入后端 | 会把后端类型暴露到公共头，违反 ADR-0002 的边界 |

## 修订记录

| 日期 | 变更 | 原因 |
| --- | --- | --- |
| 2026-11-24 | 初版（从重构规划稿与架构文档抽取） | 规划稿移出仓库 |
