# 线协议与兼容性

本文描述跨进程实际传输的字节格式。它是**协议层**的唯一权威文档；错误码语义见 [contracts.md](contracts.md)，后端实现位置见 [modules.md](modules.md)。

事实源：`src/core/rpc_frame.h`（FFRP 帧）、`src/backends/nng/registry.cc`（发现文本协议）。

## 1. 三套线格式

| 通道 | 承载 | 格式 |
| --- | --- | --- |
| Pub/Sub | 消息载荷 | 裸 Protobuf 序列化字节，一帧一条消息。**零字节是合法消息**（全默认消息） |
| RPC 请求 | `Request` 载荷 | 裸 Protobuf 序列化字节 |
| RPC 响应 | 状态 + `Response` 载荷 | `FFRP` 帧（见 §2） |
| 端点发现 | 注册/查询/广播 | 换行分隔的文本命令行，字段用 `\t` 分隔（见 §3） |

适配器不解析业务内容，也不解析响应帧里的业务 `ErrorCode`；它只搬运字节。

## 2. FFRP 响应帧

```text
偏移    长度      字段
[0..3]  4         magic   = "FFRP"（ASCII）
[4..7]  4         status  = uint32，ErrorCode 的整数值，本机字节序（memcpy）
[8..N]  N-8       payload = Response 的 Protobuf 序列化字节，可为 0 字节
```

规则：

- **本机字节序**，不带网络字节序转换。跨异构字节序部署不在支持范围内。
- `status` 的合法上界固定为 `ErrorCode::kTransport`（枚举序位置）。超出即判帧非法。
- `kCancelled` 是**本地终态**，不进入协议帧，因此不随其加入而放宽上界。
- 解码失败（magic 不符、长度 < 8、status 越界）一律映射为 `ErrorCode::kSerialization`。
- 为什么需要 `status`：早期实现用「空响应帧」表示失败，客户端无法区分业务失败、超时、服务不存在与反序列化失败，全部折叠为 nullptr。

## 3. 发现文本协议

命令行以 `\n` 结尾，字段以 `\t` 分隔。`kind` 取 `writer` / `reader` / `service`。

### 3.1 请求

| 命令 | 字段 | 说明 |
| --- | --- | --- |
| `REG` | `<kind>\t<name>\t<address>\t<id>\t<backend>` | 注册。旧版为四字段 `<kind>\t<name>\t<address>` |
| `UNREG` | `<kind>\t<name>\t<address>\t<id>` | 注销，只按 `id` 生效 |
| `QLIST` | `<kind>\t<name>\t<backend>` | 列记录，`backend` 为空表示不过滤 |
| `LIST` | `<kind>\t<name>` | 旧版列地址；`QLIST` 不可用时回退 |
| `HAS` | `<kind>\t<name>` | 是否存在记录 |

### 3.2 响应

| 场景 | 响应 |
| --- | --- |
| 新版注册成功 | `OK <id>\n` |
| 旧版注册请求 | `OK\n`（客户端按 `kind+name+address` 派生稳定兼容 id） |
| `QLIST` / `LIST` | `OK <count>\n`，随后每行 `<id>\t<backend>\t<address>\n` |
| `HAS` | `OK 1\n` / `OK 0\n` |
| 失败 | 空响应或非 `OK` 前缀，调用方按传输错误处理 |

### 3.3 角色与一致性

- 第一个绑定发现地址的进程成为 **Primary**：提供 REP 查询服务，并经 PUB 广播变更。
- 其余进程成为 **Follower**：经 REQ 查询、经 SUB 接收广播。Follower 首次 Watch 主动取快照，并用本地修订号保护「查询期间已收到的增量」不被旧快照覆盖。
- 定期对账间隔 **3 秒**：以 `QLIST` 的完整快照为准做双向对账（补漏报的新增、删漏报的注销）。`QLIST` 失败与「`OK 0`」语义不同——前者保留本地记录，后者清空该键。
- 角色在启动时确定，无运行中切换、无进程租约回收。

## 4. 兼容性承诺

| 对象 | 承诺 |
| --- | --- |
| Pub/Sub 载荷 | 裸 Protobuf 字节格式不变。字段增删遵循 [contracts.md](contracts.md) §9 |
| FFRP 帧 | 魔数、字段顺序、`status` 语义与上界不变。新增状态码只能追加在 `kTransport` 之前并同步放宽上界 |
| 发现文本协议 | 新字段**只允许追加在行尾**，旧版必须能忽略。已追加：`id`、`backend`；`QLIST` 不可用时新版回退 `LIST` |
| 公共 API | 保持公开头路径、`firefly::firefly`、端点创建方式与公开结果语义。**源码兼容**为范围，无二进制 ABI 承诺（G11） |
| 构建 | `FIREFLY_WITH_NNG` 为可选开关，默认 ON，不改变既有构建方式 |

判据：跨版本互通由 `cross_*` 四组用例守护（`legacy_pub→new_sub`、`new_pub→legacy_sub`、`legacy_rep→new_req`、`new_rep→legacy_req`），见 [contracts.md](contracts.md) 附录 A。

## 5. 改协议的流程

1. 先写 ADR（跨模块契约变更，见 [decisions/README.md](../decisions/README.md) 的准入条件）。
2. 改本文件与 `src/core/rpc_frame.h` 或 `src/backends/nng/registry.cc`，**同一 PR**。
3. 补跨进程用例，并确认旧版进程仍能互通；不能互通时必须在 ADR 中写明破坏窗口与迁移方式。
4. 更新 [known-gaps.md](known-gaps.md)：若引入新的未闭合项，加一行。
