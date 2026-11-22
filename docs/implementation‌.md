对 Firefly，我建议把替换点放在 **Reader、Writer、Client、Service 的 Core 下面**，同时把 **数据传输** 和 **端点发现** 分成两个独立契约。上层继续使用现有 C++ API，Core 负责 Firefly 的通信语义，后端负责实现具体的收发和连接。

当前的 `ISocket` 已经挡住了公共头里的 NNG 类型，但 Core 仍得理解 `kPub/kSub/kReq/kRep`、`Bind/Dial`、socket 超时和整数错误码。`WriterCore` 甚至直接判断 `NNG_EAGAIN`，默认 Registry 也直接使用 NNG。真正能替换后端的边界，需要把这些依赖一起收进去。 [现有接口](https://github.com/sunwenqi0713/Firefly/blob/4aecd3219c8e2d103944989da2432482c4116699/firefly/detail/interfaces.h#L23-L52) · [WriterCore](https://github.com/sunwenqi0713/Firefly/blob/4aecd3219c8e2d103944989da2432482c4116699/firefly/detail/writer_core.cc#L34-L40) · [Registry](https://github.com/sunwenqi0713/Firefly/blob/4aecd3219c8e2d103944989da2432482c4116699/firefly/detail/discovery.cc#L268-L297)

## 分层与职责
![](https://cdn.nlark.com/yuque/__mermaid_v3/d3953c050143c584ba9bb98fa7c3d265.svg)

| 层 | 职责 |
| --- | --- |
| 公共 API | 保持 `CreateXxx`、`Write`、`SendRequest`、`RequestHandle` 和回调签名。模板层继续完成 Protobuf 编解码。 |
| Firefly Core | 定义请求排队与取消、结果兑现、Reader 缓冲、回调执行、统计和关停语义。保持现有同一 Reader 或 Service 的回调串行约定。 |
| 数据传输后端 | 负责监听、连接、按消息收发、解除阻塞，以及把后端错误转换为 Firefly 内部的传输状态。 |
| 端点发现 | 负责注册、注销、查询和订阅变化。每个端点拥有独立 ID，地址作为供对应后端解释的数据。 |


## 用四个角色接口替换通用 `ISocket`
当前 Core 已经按四种端点拆开，可以直接利用这个结构。内部工厂返回四类对象，接口只提供该角色需要的操作。

| 内部接口 | 最小操作 | Firefly Core 保留的行为 |
| --- | --- | --- |
| `Publisher` | 获取监听地址、`Send(message)`、`Close()` | `WriteStatus`、读者存在性检查、发布统计 |
| `Subscriber` | 更新发布者地址集合、`Receive(deadline)`、`Close()` | 发现事件处理、Observe 缓冲、回调和丢弃统计 |
| `RpcRequester` | `Exchange(endpoint, request, deadline)`、`Close()` | 有界请求队列、服务选择、发送前取消、响应结果兑现 |
| `RpcResponder` | 获取监听地址、`Receive(deadline)`、`Reply(token, response, deadline)`、`Close()` | 调用服务回调、编码响应、处理业务错误 |


这里有几个值得一开始就定下来的契约：

+ **RPC 以一次交换为单位。** Core 给后端一个绝对截止时间，排队、连接、发送、接收共用这份预算。Core 在发送前明确判定取消与发送谁先发生。发送已经开始后，取消可以结束本地等待，服务端仍可能执行。对设备命令，重发策略也应显式定义，避免换后端时改变动作的执行次数。
+ **回复用不透明的 **`ReplyToken`**。** Core 只把 token 交回收到该请求的后端。NNG 可以在适配器内维护 REP 的收发状态，其他后端可以用连接句柄或请求 ID。
+ **传输状态用内部枚举。** 例如 `kOk`、`kTimeout`、`kClosed`、`kWouldBlock`、`kIoError`。`kNoSubscriber` 继续由 Core 根据发现结果判定，`kAccepted` 表示本地发送被接受。NNG 错误码留在适配器内。
+ `Close()`** 要能唤醒阻塞 I/O。** Core 才能先停止接纳新工作，再唤醒 I/O、等待工作线程退出，并完成端点注销。每个后端都要满足这条生命周期契约。

## 发现层也要能单独替换
建议保留 `IRegistry`，把记录调整为 `EndpointRecord { id, kind, name, backend, address }`。**按 **`id`** 注销，按类型和逻辑名称查询。** 两个同通道 Reader 即使都没有监听地址，也各有独立记录，可以直接解决当前删一个 Reader 就误删整个通道订阅关系的问题。

由运行时组装默认的 `NngTransportFactory + NngRegistry`，测试继续通过现有注入点替换其中任一侧。开发第二个数据后端时，可以先沿用 NNG Registry；如果目标是让部署包彻底移除 NNG，则还需要替换 Registry，并在构建时排除这两个 NNG 实现。`backend` 字段可用于过滤不兼容的端点地址。若以后要求 NNG 进程与新后端进程直接互通，还需要共同的线上协议或桥接。

## 在当前仓库里怎么落地
1. **收口端点行为。** 把 RPC 的发送前取消、截止时间和端点关停写成可重复的 Core 契约测试。复用已有 fake 注入，覆盖多个 Reader 的独立注册、Service 释放，以及回调和关停交错。对当前单在途的 RPC，一个有界队列加一个执行者就够。
2. **抽角色接口，迁移 NNG 适配器。** 保持公共 API，删除 Core 对 `<nng/nng.h>` 和 NNG 错误码的引用。让内存后端和 NNG 后端跑同一组契约测试，再用跨进程用例验证发现与实际收发。将 Registry 的监听地址做成可配置项，测试各用独立地址。

验收可以很具体：**同一套上层示例只切换运行时组装就能运行；仅构建 Core 和内存后端时，无需 NNG 头文件或库；换后端后，取消、超时、回调顺序和关停行为仍通过同一组测试。**

****

建议分 **4 批**实施，每批都能独立构建和验收。顺序是 **修正端点行为 → 抽取传输接口 → 整理发现与运行时组装 → 验证无 NNG 构建**。先用现有 NNG 后端确定行为基线，拆接口时就能判断哪些差异是有意修复，哪些是迁移引入的回归。

## **第 1 批：修正端点生命周期和 RPC 行为**
**这一批沿用现有 **`**ISocket**`** 和注入点，集中处理审查中已确认的问题。**

| **位置** | **实施动作** | **验收** |
| --- | --- | --- |
| `**service.h**`**、**`**service_core.cc**` | **让 Service 的公开对象释放时触发停机。统一显式 **`**Shutdown()**`** 和析构的清理路径，确保停止接收、工作线程退出和注销各执行一次。明确回调线程内的停机限制，避免析构路径自 **`**join**`**。** | **释放最后一个外部引用后，服务注销、线程退出，客户端无法再调用旧服务。** |
| `**node.cc**`**、**`**reader_core.cc**` | `**DeleteReader()**`** 在锁内标记端点正在删除，释放节点锁后执行 **`**Shutdown()**`**，完成后释放名称。这样回调仍可查询 Node，同名 Reader 也要等旧端点退出后才能创建。** | **回调内并发 **`**GetReader()**`** 时，删除能够返回。删除完成后可以重建。** |
| `**client.h**`**、**`**client_core.cc**` | **同步和异步请求共用一个有界队列和单个 I/O 执行者。**`**RequestHandle**`** 捕获所属 Core 的弱引用。请求入队时计算 **`**steady_clock**`** 截止时间，执行者从队列取出后检查期限，并在提交发送前用同一状态转换裁决取消与发送。** | **排队期间取消或到期，服务端执行次数为 0。每个请求只兑现一次。Client 销毁或重新初始化后，旧句柄的 **`**Cancel()**`** 返回 **`**false**`**。** |


**RPC 建议区分****排队中****和****已提交发送****。排队中的请求取消成功后必须从发送路径移除。已提交发送的请求仍可能被服务端执行，取消只影响本地结果。队列满时，先用现有错误码明确返回失败，避免顺手扩大公共 API。同步和异步调用共享同一个截止时间模型，服务查询、连接和收发都得受预算约束，否则仍可能出现短超时请求长时间挂起。**

## **第 2 批：按角色迁移 NNG 数据传输**
**从 **`**firefly/detail/interfaces.h**`** 和 **`**transport_nng.cc**`** 入手，按 ****Writer/Reader → Client/Service**** 的顺序迁移。先跑通一条发布订阅链路，再迁移请求应答。**

1. **在内部工厂增加四个角色接口。**** **`**Publisher**`** 提供监听地址和发送，**`**Subscriber**`** 接收消息并更新发布者地址集合，**`**RpcRequester**`** 提供一次请求应答交换，**`**RpcResponder**`** 通过不透明 **`**ReplyToken**`** 回应请求。沿用当前字符串字节载荷即可。**
2. **把协议和错误差异收进适配器。**** NNG 的 socket 类型、连接和重连、收发状态、超时设置以及 **`**NNG_EAGAIN**`** 映射都放进 **`**transport_nng.cc**`**。Core 只接收统一的 I/O 状态。**`**RpcRequester**`** 接收绝对截止时间，适配器每次开始阻塞操作前重新计算剩余时间。**
3. **明确 **`**Close()**`** 的并发契约。**** 它必须幂等，并且能从另一线程唤醒正在阻塞的 I/O。适配器负责句柄同步，Core 负责停止接纳、唤醒 I/O 和回收线程。迁移时保持现有消息载荷和 RPC 响应帧格式，并用旧版与新版进程互测。**

**验收****：四个 Core 均无 NNG 头文件、常量或错误码引用。旧 **`**ISocket**`** 的生产调用者全部迁移后再删除接口。现有 NNG 示例、原有测试和新增的生命周期用例全部通过。**

## **第 3 批：拆开端点发现与默认组装**
**这批同时解决 Registry 的地址耦合和生命周期问题。**

| **改动** | **具体做法** |
| --- | --- |
| **端点身份** | **在 **`**IRegistry**`** 中引入 **`**EndpointRecord**`**，按 **`**kind + name**`** 查询，按独立的 **`**EndpointId**`** 注销。Reader 没有监听地址也能单独登记。默认 Registry 若需要更新线协议，先明确新旧进程是否需要混跑，并按该要求验收。** |
| **发现更新** | **Reader 通过 **`**Watch**`** 拿到一致的初始快照和后续变更。默认发现使用广播时，要能在启动、重连或漏事件后重新查询并对账。Core 把当前地址集合交给 **`**Subscriber**`**，由后端处理重复更新和连接去留。** |
| **启动和销毁** | **端点完成监听后再注册。注册失败时关闭已创建的资源，并通过现有 **`**Result**`** 返回失败。注册中心停止和重新启动后，明确旧记录的清理规则，并覆盖 **`**Clear()**`** 后端点迟到注销的情况。** |
| **默认依赖** | **在运行时入口一次组装 **`**NngTransportFactory**`** 和默认 Registry。四个 Core 接收已经解析好的依赖，移除各自选择默认后端的代码。继续复用现有 **`**InjectionContext**`** 测试入口。** |


**验收****：两个同通道 Reader 分属不同 Node，删掉一个后另一个仍可收到消息。Reader 启动期间新增 Writer 能最终发现。**`**Clear()**`**、销毁旧端点、再次 **`**Init()**`** 后查询不到旧记录。注册失败时，工厂返回错误且无残留工作线程或监听器。**

## **第 4 批：用构建和契约测试证明可替换**
+ **拆 CMake 依赖。**** 保留现有默认 **`**firefly::firefly**`** target，内部把 Core 与 NNG 实现分开。关闭 NNG 时，配置阶段跳过 **`**find_package(nng)**`**，编译阶段排除 NNG 数据传输和 NNG Registry，运行时用内存实现组装验证。**
+ **共享契约测试。**** 复用 **`**fake_transport_test.cpp**`** 的注入思路，让内存和 NNG 跑同一套可观察行为测试。重点检查取消、截止时间、**`**Close()**`** 唤醒、响应对应关系、回调顺序和独立注册。用屏障或 promise 控制并发时序。**
+ **补跨进程覆盖。**** 用两个进程验证 NNG 的发现、Pub/Sub 和 RPC。让每个测试使用独立的发现地址，再跑并行 CTest，避免仓库当前固定端口造成的相互干扰。**

**完成标准是同一组上层调用可以分别在默认 NNG 组装和内存组装下运行，两者通过同一套行为契约，并且无 NNG 配置能在未安装 NNG 的环境中完成构建、链接和测试。做到这一步，接入下一套生产后端才有清楚的实现范围和验收基线。**


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
