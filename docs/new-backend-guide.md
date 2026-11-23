# 接入新生产后端指南

> 适用对象：为 Firefly 增加**第二套生产级**数据传输 / 端点发现后端。
> 前置阅读：`docs/build.md`（target 拓扑与验证入口）、
> `docs/acceptance-matrix.md`（验收基线）、
> `src/core/interfaces.h`（唯一接口定义）、
> `include/src/core/endpoint_api.h`（四角色行为契约表）。

## 0. 一句话原则

**后端 = 实现 `detail` 层接口 + 提供默认注入 + 通过同一套契约测试；Core 一行不改。**

四批重构已把「通信语义」与「传输/发现实现」切开：四个 Core 只认四角色接口
与 `IRegistry`，公共 API 与线协议不感知后端。新后端接入是**加实现**，不是
**改语义**。

## 1. 最小实现面

| # | 组件 | 接口 / 函数 | 参考实现 |
| --- | --- | --- | --- |
| 1 | 四角色工厂 | `IEndpointFactory` | `memory_transport.cc` / `src/backends/nng/transport.cc` |
| 2 | 发布者 | `Publisher` | 同上 |
| 3 | 订阅者 | `Subscriber` | 同上 |
| 4 | RPC 请求方 | `RpcRequester` | 同上 |
| 5 | RPC 应答方 | `RpcResponder` | 同上 |
| 6 | 注册中心 | `IRegistry`（记录模型） | `memory_registry.cc` / `discovery.cc` + `nng_injection.cc` |
| 7 | 默认注入函数组 | 见 1.3 | `memory_injection.cc` / `nng_injection.cc` |
| 8 | 显式构造导出 | `backend_access.h` 增加两行声明 | `backend_access.h` |

所有类型都定义在 `firefly::detail` 命名空间，位于 `src/core/` 下；
**不得**出现在任何公共头（`include/firefly/firefly.h`、`include/firefly/node/*`、`include/firefly/service/*`、
`firefly/common/*`）中。

### 1.1 四角色签名与必须满足的契约

```cpp
class Publisher {
  virtual TransferStatus Bind(std::string* endpoint) = 0;   // 写回不透明监听地址
  virtual TransferStatus Send(const std::string& bytes) = 0; // 一帧；不保证对端收到
  virtual void Close() = 0;                                  // 幂等、跨线程唤醒
};
class Subscriber {
  virtual TransferStatus UpdatePublishers(
      const std::vector<std::string>& endpoints) = 0;        // 当前地址全集
  virtual TransferStatus Receive(std::string* bytes, Deadline deadline) = 0;
  virtual void Close() = 0;
};
class RpcRequester {
  virtual TransferStatus Exchange(const std::string& endpoint,
                                  const std::string& request, Deadline deadline,
                                  std::string* response) = 0;
  virtual void Close() = 0;
};
class RpcResponder {
  virtual TransferStatus Bind(std::string* endpoint) = 0;
  virtual TransferStatus Receive(std::string* request,
                                 std::unique_ptr<ReplyToken>* token,
                                 Deadline deadline) = 0;
  virtual TransferStatus Reply(std::unique_ptr<ReplyToken> token,
                               const std::string& frame, Deadline deadline) = 0;
  virtual void Close() = 0;
};
class IEndpointFactory {
  virtual std::unique_ptr<Publisher> CreatePublisher() = 0;
  virtual std::unique_ptr<Subscriber> CreateSubscriber() = 0;
  virtual std::unique_ptr<RpcRequester> CreateRequester() = 0;
  virtual std::unique_ptr<RpcResponder> CreateResponder() = 0;
};
```

逐条不可打折的契约（完整表见 `endpoint_api.h` 第 23–71 行）：

- `Deadline` 是 `std::chrono::steady_clock::time_point` **绝对时间点**。
  `Exchange` 在连接 / 发送 / 接收每个阻塞点**之前**重算剩余预算，任一步耗尽即
  `kTimeout`；不得把传入值再加一次超时，也不得各步骤共用一次算出的剩余毫秒数。
- `UpdatePublishers` 收到的是**当前地址全集**：适配器自行去重、建连、断开已移除
  地址；重复传入同一集合必须是 no-op；空串地址忽略。
- `ReplyToken` **不透明**：只有虚析构函数，具体类型留在适配器内部。Core 只把
  `Receive` 产出的 `unique_ptr` 原样交给 `Reply`（或让它在超时路径析构），
  适配器负责 token 与请求一一对应、只消费一次。
- `Close()` 对四个角色都**幂等且可跨线程调用**：返回后任何阻塞 I/O 已被唤醒并
  以 `kClosed` 结束；此后所有新调用立即返回 `kClosed`。
- 所有后端错误**必须收口**到 `TransferStatus`
  （`kOk/kTimeout/kClosed/kWouldBlock/kIoError/kInvalidArgument`）。
  后端专有错误码（如 `NNG_EAGAIN`）不得跨过适配器边界；`Core` 侧映射见
  `endpoint_api.h` 第 30–37 行。
- 载荷是**字节串**：Pub/Sub 为序列化后的 protobuf，RPC 为 `FFRP` 帧。适配器
  **不解析**业务内容、不解析响应帧里的业务 `ErrorCode`。

### 1.2 `IRegistry` 记录模型

```cpp
struct EndpointRecord {
  EndpointRecordId id = 0;      // 注册返回的不透明句柄
  RegistryKind kind = ...;     // kWriter / kReader / kService
  std::string name;             // 逻辑通道 / 服务名
  std::string backend;          // 后端标识，供 Query 过滤
  std::string address;           // 监听地址；Reader 以空串占位
};
class IRegistry {
  virtual EndpointRecordId Register(const EndpointRecord&) = 0; // 成功非零，失败 0
  virtual bool Unregister(EndpointRecordId id) = 0;             // 只按 id
  virtual std::vector<EndpointRecord> Query(
      RegistryKind, const std::string& name,
      const std::string& backend = std::string()) = 0;
  virtual bool Has(RegistryKind, const std::string& name) = 0;
  virtual int Watch(RegistryKind, const std::string& name, WatchCallback&& cb) = 0;
  virtual void Unwatch(int id) = 0;
  virtual void Clear() = 0;
};
```

不可打折的契约（完整注释见 `interfaces.h` 第 125–153 行）：

- **按 id 注销**，同一 `kind+name` 下允许多条记录并存，`Query` 必须全部返回，
  不得互相覆盖。两个同通道 Reader 即使都用空 `address` 也各有独立记录，
  删一个不影响另一个。
- `Watch` 在注册回调的**同一串行化域**内取得一致初始快照（按 id 排序，含注册
  前已存在的记录），此后该键任何增删都派发**完整当前快照**；回调内禁止调用
  `IRegistry`；`Unwatch` 后不再派发。
- `Clear()` 幂等：停线程/监听器、清空记录与订阅；之后迟到的
  `Unregister(id)` 为 no-op 并返回 `true`；再次 `Register`/`Watch` 可重启且
  看不到旧记录。注册失败不得留下线程或监听器。
- 若需要与既有 NNG 进程混跑，线协议必须兼容：注册/注销命令与广播**在末尾追加**
  可选 `id`/`backend` 字段，旧版忽略新字段；新版对缺失字段按
  `kind+name+address` 派生稳定兼容 id；`QLIST` 不可用时回退 `LIST`。
  详见 `docs/acceptance-matrix.md` 3.7 与已知限制 L2。

### 1.3 默认注入函数组（每个后端各实现一次，最终链接只出现一份）

在 `src/core/<name>_injection.cc` 中实现 `interfaces.h` 声明的全部入口：

| 函数 | 职责 |
| --- | --- |
| `ConfigureDiscovery(const DiscoveryConfig&)` | 首次 `Init` 前设置发现地址；空串保留当前值 |
| `BootstrapDefaultInjection()` | `Init()` 调用一次，组装 `DefaultEndpointFactory()` + `DefaultRegistry()` |
| `ResetDefaultInjection()` | `Clear()` 调用，清空默认 ctx 并 `registry->Clear()` |
| `DefaultInjection()` | 懒组装兜底，返回当前默认 ctx |
| `ResolveInjection(ctx)` | 只补齐 `ctx` 中为空的字段，**不做后端选择** |
| `DefaultEndpointFactory()` / `DefaultRegistry()` | 返回本后端的工厂与注册中心（函数级静态） |

注意：这些符号**不能**在两个后端 object 中同时进入最终链接。第 4 批因此把后端
拆成「角色 object」与「注入 object」两层：角色 object 可被同一测试进程显式
链接多次，注入 object 每个后端各一份、聚合库只选其一。参见 `CMakeLists.txt`
`configure_firefly_object()` 与 `FIREFLY_BACKEND_OBJECTS`。

### 1.4 `backend_access.h` 导出

为共享契约测试增加显式构造入口：

```cpp
std::shared_ptr<IEndpointFactory> Make<Name>EndpointFactory();
std::shared_ptr<IRegistry> Make<Name>Registry();
```

这两个函数返回**新的显式实例**，不参与默认注入选择；测试据此在同一进程内同时
构造两个后端而不需要切换全局默认。

## 2. 构建接入（不改 target 拓扑）

1. 新增 `src/core/<name>_transport.cc`、`<name>_registry.cc`、
   `<name>_injection.cc`（若注册中心与传输紧耦合，可合并，但保持「角色」与
   「注入」两个 translation unit 的拆分）。
2. 在 `CMakeLists.txt` 增加 `<name>_role_objects` 与 `<name>_injection_objects`
   两个 `OBJECT` 库，复用 `configure_firefly_object()`，并声明该后端自身的链接
   依赖。**不新增第三方依赖**（本里程碑约束；若确需新依赖，须单独评审并同步
   `vcpkg.json` 与 CI 前缀构造脚本）。
3. 用 `FIREFLY_WITH_<NAME>` 之类的开关控制是否与 NNG / 内存后端互斥地注入；
   `firefly::firefly` 仍为 Core object + **一个**后端 object 的聚合，
   `firefly_core` 仍自包含且不链接任何后端专有库。
4. `scripts/verify.sh` 的无 NNG 独立前缀方案按新依赖调整：从
   `build/vcpkg_installed` 只复制非 NNG 内容，且 OFF 构建目录与
   `CMAKE_PREFIX_PATH` 绑定（已知限制 L11）。

## 3. 必过的契约测试集

| 测试 | 覆盖 | 新后端接入要求 |
| --- | --- | --- |
| `tests/contract/backend_contract_test.cpp` **双实例** | 取消、绝对截止时间、`Close()` 跨线程唤醒与幂等、响应对应、回调顺序、独立注册（6 case） | 同一份源码增加 `<name>` 分支并注册 CTest 实例；与既有 `backend_contract_memory` / `backend_contract_nng` 并列，**不得复制测试逻辑** |
| `batch1_contract_test` | Service 停机、Reader 删除并发、RPC 队列/取消 | 确认 Core 行为不回归 |
| `batch3_contract_test` | 记录模型、多记录/backend 过滤、Watch 快照、Clear 重入、注册失败无残留 | 新 `IRegistry` 必须全绿 |
| `fake_transport_test` / `no_leak_check` | 角色状态映射、公共头不泄漏后端实现 | 新后端不得让 `no_leak_check` 失败 |
| **跨进程双向**（`interop_helper` + `run_interop_pair.py`） | 发现、Pub/Sub、RPC 的线兼容 | 4 组：`legacy_pub→new_sub`、`new_pub→legacy_sub`、`legacy_rep→new_req`、`new_rep→legacy_req`；每组独立发现端口、`*_READY` 启动屏障、并行 CTest 稳定 |
| 默认 ON 全量 `ctest -j6` / OFF 全量 | 整体回归 | `bash scripts/verify.sh` 一条命令 |

跨进程用例的已知偶发（follower 注册 3 秒超时，L10）由 `verify.sh` 做最多
3 次有限重试；**不要**通过放宽断言或删除用例来「修绿」。

## 4. 「不碰 Core」边界（红线）

允许修改的只有：

- `src/core/` 下新增的后端实现与注入文件；
- `src/core/backend_access.h` 的导出声明；
- `CMakeLists.txt` 的后端 object 与开关（**保持 target 拓扑不变**）；
- `tests/` 下的新后端测试实例与互测辅助程序；
- `docs/` 文档。

**禁止**：

- 修改 `src/core/*_core.cc`（四个 Core）、`include/firefly/node/*`、
  `include/firefly/service/*`、`firefly/common/*`、任何公共头；
- 修改公共 API 签名、`WriteStatus` / `ErrorCode` / `RequestHandle` 语义、
  `QosProfile`、回调执行模型与关停契约；
- 修改线协议：Pub/Sub 裸 protobuf 载荷、RPC `FFRP` 响应帧、发现文本协议；
- 修改 `FIREFLY_WITH_NNG` 默认语义、`firefly::firefly` 聚合方式或
  `firefly_core` 的零 NNG 属性；
- 把后端专有类型、常量、错误码或头文件（如 `<nng/...>`）泄漏进
  `interfaces.h` / `endpoint_api.h` / 公共头；
- 用「顺手优化 Core」的方式绕过契约差异：若新后端无法满足某条契约，
  应先记录为已知限制并评审，而不是改 Core 去迁就后端。

**互通限制（L2）**：新后端进程与既有 NNG 进程直接互通需要共同的线上协议或
桥接，当前不在实现范围内；`backend` 字段可用于过滤不兼容的端点地址。

## 5. 建议实施顺序

1. 先用进程内实现（参考 `memory_transport.cc` / `memory_registry.cc`）跑通
   `backend_contract_test` 的 6 个 case 与批 3 记录模型测试；
2. 接入真实网络/发现，补 `Close()` 唤醒与绝对 deadline 的阻塞点重算；
3. 用 `interop_helper` 补跨进程双向 4 组，独立端口 + READY 屏障；
4. 跑 `bash scripts/verify.sh` 全量，确认默认 ON 30/30 与无 NNG OFF 路径无回归；
5. 更新 `docs/acceptance-matrix.md` 与发布说明，记录新后端的覆盖范围与限制。

## 6. 自检清单

- [ ] 四角色 + `IEndpointFactory` 全部实现，错误码统一收口到 `TransferStatus`；
- [ ] `Close()` 幂等且跨线程唤醒；`Exchange` 使用绝对 deadline 且逐阻塞点重算；
- [ ] `ReplyToken` 不透明且不跨请求复用；
- [ ] `IRegistry` 支持按 id 注销、多记录、backend 过滤、Watch 完整快照、
      Clear 后迟到注销 no-op、注册失败无残留；
- [ ] 注入函数组 7 个入口齐备，角色/注入 object 拆分，最终链接只有一份注入符号；
- [ ] `backend_access.h` 导出 `Make<Name>EndpointFactory` / `Make<Name>Registry`；
- [ ] `backend_contract_test` 新实例与既有双实例并列通过；批 1/批 3 全绿；
- [ ] 跨进程双向 4 组通过，`ctest -j6` 稳定；
- [ ] `bash scripts/verify.sh` 一条命令全绿，公共 API / 线协议 / target 拓扑未变；
- [ ] `no_leak_check` 通过，公共头未出现后端专有类型或头文件名。
