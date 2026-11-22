> 本文记录四批重构中"规划 vs 实际"的落地差异，供实现参考；
> 规划原文见 `docs/implementation.md`，验收基线见 `docs/acceptance-matrix.md`。

## 第 2 批落地说明（角色接口，2026-11-21）

本批把数据传输从通用 `ISocket` 迁移到四个角色接口，声明与契约表位于
`firefly/detail/endpoint_api.h` 顶部，接口定义位于 `firefly/detail/interfaces.h`。

| 角色 | 适配器实现 | Core 保留行为 |
| --- | --- | --- |
| `Publisher` | NNG PUB；`Bind` 临时端口、非阻塞 `Send` | `WriteStatus` 映射、`kNoSubscriber` 判定、统计 |
| `Subscriber` | NNG SUB；地址集合去重、非阻塞轮询 + 条件变量等待 | 发现事件、Observe 缓冲、回调串行、统计 |
| `RpcRequester` | NNG REQ；建连/失配重建、绝对 deadline 下每段剩余预算重算 | 有界队列、单执行者、`PendingCall` 唯一裁决、帧解码 |
| `RpcResponder` | NNG REP；不透明 `ReplyToken`、按帧回送 | 业务回调、FFRP 帧编码、`ErrorCode` 映射 |

契约要点：
- `TransferStatus`：`kOk/kTimeout/kClosed/kWouldBlock/kIoError/kInvalidArgument`。
  `NNG_EAGAIN` 只在 `transport_nng.cc` 映射为 `kWouldBlock`，Core 只见统一状态。
- `Exchange(endpoint, request, deadline, response)` 收 `steady_clock` 绝对时间点；
  连接、发送、接收各阻塞点之前都重新计算剩余预算，任一步耗尽即 `kTimeout`。
- `ReplyToken` 是只有虚析构的不透明接口；token 由适配器生成并校验归属，
  Core 只把 `Receive` 得到的 token 原样交给 `Reply`，不解析其内部。
- 四个角色的 `Close()` 幂等且可跨线程唤醒阻塞 I/O；此后所有调用返回 `kClosed`。
  假后端与真实 NNG 后端均有 `future`/条件变量控制的唤醒与期限测试。
- 第 1 批 `PendingCall` 状态机保持为 Core 侧唯一“取消 vs 发送”裁决点；
  适配器不感知取消状态，不参与是否发送的判定。

收尾：`ISocket`/`ITransportFactory`/`kIo*` 整数码已删除；四个 Core 不再引用
任何 NNG 头、常量或错误码。`firefly_interop_helper` 的 8 个模式覆盖旧版 raw-NNG
进程与新版公开 API 进程的 Pub/Sub、RPC 双向互测，载荷与 `FFRP` 响应帧格式不变。
## 第 3 批落地说明（发现层与运行时组装，2026-11-21）

发现层从"按 kind+name 的单值表 + 空串引用计数"改为记录模型：
`EndpointRecord{id,kind,name,backend,address}`，`id` 为注册返回的不透明
句柄；注销只按 id，查询按 kind+name 返回全部记录并可按 backend 过滤。
Reader 没有监听地址，仍以空 address 独立登记，删一个不影响同通道其他 Reader。

`Watch` 在注册回调的同一串行化域内取得一致初始快照，此后每次增删派发该键
的完整快照；回调内不得调用 IRegistry。Clear 幂等，清空记录与订阅，之后
迟到的 Unregister(id) 为 no-op，再次 Register/Watch 可重启且无旧记录。

线协议仅在 REG/UNREG 与广播末尾追加可选 id/backend 字段：旧版进程忽略新
字段，新版对缺失字段按 kind+name+address 派生稳定兼容 id，QLIST 不可用时
回退旧 LIST 文本协议。FFRP 响应帧与 protobuf 载荷格式保持不变。

四 Core 不再各自选择默认后端：Init 在运行时入口一次组装 NngTransportFactory
与 NngRegistry，`ResolveInjection` 只补齐空字段，测试仍可注入 Fake。发现地址
可通过 `detail::ConfigureDiscovery` 或环境变量 FIREFLY_DISCOVERY_REP_URL /
FIREFLY_DISCOVERY_PUB_URL 在首次使用前配置；测试进程各自选空闲端口。
`Subscriber::UpdatePublishers` 接收当前发布者地址全集，NNG 适配器负责去重、
建连与断开已移除地址。启动顺序恒为 Bind → Register → 启动工作线程；注册
失败即 Close 已创建端点并返回 kTransport。
