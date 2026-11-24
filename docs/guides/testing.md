# 测试与验证

测试分组、运行入口与新增测试的规则。**契约与测试用例的对应关系只在 [../architecture/contracts.md](../architecture/contracts.md) 附录 A 维护**，本文不复述。

## 1. 目录与职责

| 路径 | 内容 |
| --- | --- |
| `tests/contract/` | 行为契约。`backend_contract_test.cpp` 是唯一跨后端共享的契约测试；`concurrency_contract_test`（生命周期/RPC 队列）、`registry_contract_test`（发现记录模型）、`fake_transport_test`（角色状态映射）、`creation_failure_test`、`endpoint_lifecycle_test` |
| `tests/integration/` | 端到端与跨进程。`e2e_test`、`reader_test`、`client_test`、`discovery_test`、`interop_helper.cpp` + `run_interop_pair.py` |
| `tests/support/` | `test_messages.proto` 等共享消息定义 |
| `tests/`（根） | 单点语义：`error_code`、`write_status`、`message_traits`、`callback_context`、`callback_contract`、`stats`、`shutdown`、`clear_wait`、`duplicate`、`empty_message`、`request_cancel`、`rpc_error`、`deprecated_compat`、`header_check`、`review_regression` |

约定：**文件名不用批次号、阶段号**；新增测试按语义命名（`<主题>_test.cpp`）。

## 2. 运行

```bash
# 全量
ctest --preset default

# 并行（CI 与验收口径）
ctest --test-dir build -j6 --output-on-failure

# 只跑某个用例或某组
ctest --test-dir build -R '^backend_contract'
ctest --test-dir build -R '^cross_'
```

- 无 NNG 构建：`ctest --test-dir build-mem --output-on-failure`。
- 契约用例失败时以 `*_FAILED at line N` 定位；`--output-on-failure` 必加。
- 涉及发现端口的用例带 `RESOURCE_LOCK firefly_registry_port`，不要在 `-j` 并行时移除该属性。

## 3. 分组语义

| 分组 | 何时构建 | 说明 |
| --- | --- | --- |
| `core-tests` | 两种开关 | 不直接依赖 NNG；OFF 时默认注入为内存后端 |
| `nng-tests` | 仅 ON | `discovery_test`、`interop_helper`、共享契约的 NNG 实例 |
| `shared-backend-contract` | 内存实例始终；NNG 实例仅 ON | `backend_contract_memory` / `backend_contract_nng` 两份实例 |
| `cross-process` | 仅 ON | 4 个两进程用例 |

`registry_contract_test` 两种开关都构建，NNG 相关部分用 `FIREFLY_WITH_NNG` 条件编译。

## 4. 共享契约测试的机制

`tests/contract/backend_contract_test.cpp` 是**一份源码两个实例**：`argv[1]` 选择后端，通过 `backend_access.h` 的 `Make<Nng|Memory>EndpointFactory()` / `Make<Nng|Memory>Registry()` 显式构造，不依赖 `Default*` 的后端选择。

**新增后端时不许复制这个文件**，只加一个后端分支并在 CMake 注册新实例（见 [adding-backend.md](adding-backend.md)）。复制会让两份断言逐渐分叉，而分叉的契约等于没有契约。

## 5. 跨进程互测

`tests/integration/run_interop_pair.py` 用标准库启动两个进程，以 stderr 的 `*_READY` 标记作为启动屏障：

| 用例 | 方向 | 端口对 |
| --- | --- | --- |
| `cross_pubsub_legacy_to_new` | 旧发布 → 新订阅 | 37501 / 37502 |
| `cross_pubsub_new_to_legacy` | 新发布 → 旧订阅 | 37503 / 37504 |
| `cross_rpc_legacy_to_new` | 旧应答 → 新请求 | 37505 / 37506 |
| `cross_rpc_new_to_legacy` | 新应答 → 旧请求 | 37507 / 37508 |

各对端口互不重叠，因此不加共享锁；`interop_helper` 提供 8 个模式供用例组合调用。

**失败处置**：`cross_pubsub_legacy_to_new` 在并行高负载下可能因 follower registry REQ 3 秒超时而缺 `LEGACY_PUB_OK`（见 [known-gaps.md](../architecture/known-gaps.md) EV1）。`verify.sh` 只对这类失败做最多 3 次有限重试并留日志。**不得**通过放宽断言、加大超时或删除用例让流水变绿。

## 6. 新增测试的规则

| 场景 | 放哪 | 必须 |
| --- | --- | --- |
| 新端点行为 / 新错误码路径 | `tests/contract/` 或根目录语义测试 | 断言具体 `ErrorCode`，不写「非零即失败」 |
| 新后端 | `backend_contract_test.cpp` 加分支 | 与既有实例并列注册，不复制逻辑 |
| 改动线协议 | `tests/integration/` + `cross_*` | 补一条旧版↔新版用例 |
| 修复已知差距 | 对应分组 | 用例名体现该场景，并在 [known-gaps.md](../architecture/known-gaps.md) 删行、在 [contracts.md](../architecture/contracts.md) 附录 A 登记 |
| 公共头变更 | `header_check` + `no_leak_check` | 两条都必须通过 |

命名与断言原则：用例名描述**可观察行为**（`service_release_stops_old_client`），不描述实现；不用 `sleep` 等待，改用 future/条件变量控制时序（`backend_contract_test` 的 deadline 用例是参考写法）。

## 7. 排障

| 现象 | 先查 |
| --- | --- |
| 链接到非预期后端 | 配置日志的 `NNG backend ENABLED/DISABLED` |
| 契约用例在 OFF 构建下 SegFault | 是否在同一构建目录切换过 `CMAKE_PREFIX_PATH`（EV2），换全新目录 |
| 跨进程用例偶发缺 `*_OK` | 是否 `-j6` 高负载（EV1）；单跑 `-R '^cross_'` 复现 |
| 发现相关用例串扰 | 是否漏了 `RESOURCE_LOCK firefly_registry_port` |
| 公共头泄漏后端符号 | `python tools/check_no_leak.py` |
