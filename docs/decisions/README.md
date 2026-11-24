# 架构决策记录（ADR）索引

> 编号单调递增、永不复用、永不重排。新增 ADR 必须同时：分配下一个未使用编号、在本文档登记一行、被至少一篇 `architecture/*` 或 `guides/*` 引用。
> 规则见 [guides/documentation.md](../guides/documentation.md) 第四步。

| 编号 | 标题 | 状态 | 日期 | 取代关系 |
| --- | --- | --- | --- | --- |
| 0000 | [模板（非 ADR）](0000-template.md) | — | — | 不参与状态校验 |
| 0001 | [用 ADR 记录架构决策](0001-record-architecture-decisions.md) | Accepted | 2026-11-24 | — |
| 0002 | [传输与发现拆为两个可替换契约](0002-split-transport-and-discovery-contracts.md) | Accepted | 2026-11-24 | — |
| 0003 | [传输按四角色建模](0003-four-role-transport-interfaces.md) | Accepted | 2026-11-24 | 取代 0002 中的 `ISocket` 部分 |
| 0004 | [发现层采用 EndpointRecord 记录模型](0004-endpoint-record-discovery-model.md) | Accepted | 2026-11-24 | — |
| 0005 | [每 Client 单 I/O 执行者、Reader/Service 串行回调](0005-single-io-executor-per-client.md) | Accepted | 2026-11-24 | — |
| 0006 | [不引入 ZeroMQ 第三后端](0006-no-third-backend-zmq.md) | Accepted（No-go） | 2026-11-24 | — |
| 0007 | [文档分层与过程记录出库](0007-document-taxonomy-and-process-exit.md) | Accepted | 2026-11-24 | — |

## 状态取值

| 状态 | 含义 | 能否据此实现 |
| --- | --- | --- |
| `Proposed` | 已提出，未定稿 | 否，先确认 |
| `Accepted` | 已采纳，当前有效 | 是 |
| `Superseded by ADR-XXXX` | 已被后续决策取代 | 否，读取代它的那篇 |
| `Deprecated` | 不再适用，且无替代 | 否 |
| `Rejected` | 提议被否决 | 否，但可读其原因避免重复提议 |
| `—` | 模板占位 | 不适用 |

状态可加括号补充说明，如 `Accepted（No-go）` 表示「决定不做」；门禁只校验括号前的主状态。
