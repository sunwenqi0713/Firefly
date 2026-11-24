# Firefly 文档索引

> 文档的唯一入口。新增、改名、删除文档必须同步本表，否则 CI 的孤儿检查会失败。
> 规范本身见 [guides/documentation.md](guides/documentation.md)。

## 按读者路径

| 我是谁 | 读什么 | 顺序 |
| --- | --- | --- |
| 第一次接触 | [README](../README.md) → [架构概览](architecture/overview.md) | 1 |
| 写业务代码 | [行为契约](architecture/contracts.md) → [构建](guides/build.md) → `docs/api/html/index.html` | 2 |
| 改 Core / 加后端 | [模块边界](architecture/modules.md) → [接入新后端](guides/adding-backend.md) | 3 |
| 遇到限制或异常 | [已知差距](architecture/known-gaps.md) → [线协议](architecture/wire-protocol.md) | 4 |
| 要发版 | [发布流程](guides/releasing.md) | 5 |
| 想知道为什么这么设计 | [ADR 索引](decisions/README.md) | 6 |

## architecture：现在是什么

| 文档 | 内容 | 维护角色 |
| --- | --- | --- |
| [overview.md](architecture/overview.md) | 定位、分层、决策摘要、术语表 | 架构 owner |
| [modules.md](architecture/modules.md) | 模块边界、依赖方向、target 与目录对应 | 构建 owner |
| [contracts.md](architecture/contracts.md) | 生命周期、并发、回调、错误码、送达语义；附录 A 契约→守护测试 | 模块 owner |
| [wire-protocol.md](architecture/wire-protocol.md) | Pub/Sub 载荷、FFRP 响应帧、发现文本协议、兼容性承诺 | 协议 owner |
| [known-gaps.md](architecture/known-gaps.md) | 唯一限制清单（含设计定位与未修项） | 架构 owner |

## guides：怎么做

| 文档 | 内容 | 维护角色 |
| --- | --- | --- |
| [build.md](guides/build.md) | 依赖、target 拓扑、开关、验证命令 | 构建 owner |
| [testing.md](guides/testing.md) | 测试分组、契约矩阵、跨进程互测、故障排查 | 测试 owner |
| [adding-backend.md](guides/adding-backend.md) | 接入新后端的实现面与红线 | Core owner |
| [releasing.md](guides/releasing.md) | 版本策略、tag 规范、Releases 模板 | 发布 owner |
| [documentation.md](guides/documentation.md) | 文档分层、Doxygen 规范、维护规则 | 文档 owner |

## decisions：当时为什么

见 [ADR 索引](decisions/README.md)。`0000-template.md` 是模板，不参与状态校验。

## 其它

| 位置 | 说明 |
| --- | --- |
| `docs/api/` | Doxygen 生成物，**不入版本控制**，由 CI 生成（见 [build.md](guides/build.md) 的文档目标） |
| `Doxyfile` | API 文档配置，随仓库维护 |
| `../CONTRIBUTING.md` | 提交与评审入口 |
