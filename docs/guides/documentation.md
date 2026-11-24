# Firefly 文档规范

> 状态：**已生效**。2026-11-24 完成第 1–4 步（建骨架、纯改名、拆分类、过程文档出库与 ADR 抽取），本文件即团队规范。
> 未完成：第 5–7 步（公共头 Doxygen 改造、CI 门禁接入、首轮审计）在后续独立 PR 中执行，见 6.1。
> 适用范围：`E:/source/Firefly` 全部 Markdown 文档、公共头注释、ADR、CI 文档门禁。
> 文中 `docs/...` 一律指当前路径；迁移前的旧路径写作 `旧: docs/xxx.md`，仅出现在分类表中作为溯源记录。

---

## 0. 迁移前盘点（2026-11-24 快照）

> 下表只用于说明每篇文档为何被归入某类，**迁移后不再维护**；当前结构见 2.1。

| 路径 | 规模 | Git 状态 | 内容性质 |
| --- | --- | --- | --- |
| `README.md` | 137 行 / 5.9 KB | 已跟踪 | 定位 + 能力表 + 快速开始 |
| `docs/architecture.md` | 194 行 / 18.4 KB | 已跟踪 | 架构决策、模块边界、生命周期、RPC、发现、传输契约、已知差距 |
| `docs/build.md` | 244 行 / 14.4 KB | 已跟踪 | 混合：构建契约 + 第 4 批验收记录（含逐条提交号） |
| `docs/acceptance-matrix.md` | 96 行 / 11.9 KB | 已跟踪 | 纯过程记录：批次验收标准 → 测试/提交号映射 |
| `docs/release-notes.md` | 152 行 / 10.3 KB | 已跟踪 | 混合：里程碑说明 + 逐批提交清单 + 兼容性声明 |
| `docs/implementation.md` | 93 行 / 12.6 KB | 已跟踪 | 原始规划稿（已被实现取代），含 GitHub 行号永久链接 |
| `docs/implementation-notes.md` | 52 行 / 4.0 KB | 已跟踪 | 「规划 vs 实际」落地差异记录 |
| `docs/new-backend-guide.md` | 237 行 / 13.9 KB | 已跟踪 | 操作指南：接入新后端 |
| `docs/eval-zeromq-backend.md` | 169 行 / 13.3 KB | **未跟踪** | 技术评估与结论（11–16 人日、建议不做） |
| `Doxyfile` | — | 不存在 | 需新建 |
| `.github/workflows/` | — | 不存在 | 需新建 |
| `docs/api/` | — | 不存在 | 需新建且不入库 |

**核心问题一句话**：现有 `docs/` 把「现在是什么」（契约）、「当时为什么这么定」（决策）、「当时做到了哪一步」（过程）三类内容混在同一批文件里，而**过程类占了一半以上**（acceptance-matrix、implementation、implementation-notes 合计 241 行，另加 build/release-notes 的过程段落）。目录只有一个层级，读者无法判断哪篇可信、哪篇已过期。

---

## 第一步：文档分类与分流建议

### 1.1 什么算「过程记录」——四条判定标准

任一文档满足**两条以上**即归入过程记录，不再作为工程文档维护：

1. **时效性绑定**：结论只在某一批次/某一时间点成立（「第 4 批」「本阶段」「T3 验收」）。
2. **价值即流水**：主要内容是提交号、日期、人名、Issue/PR 号、逐批清单。
3. **只描述「当时怎么想/做到哪」**：不回答「现在能不能这样改」。
4. **可被代码证伪**：写的是验收结果，而验收本身由 `ctest` 实时证明——文档里的快照永远落后于测试。

对照本项目，明确属于过程记录的六类：

| 过程记录类型 | 本仓库实例 | 处理方式 |
| --- | --- | --- |
| 阶段计划 / 原始规划稿 | `旧: docs/implementation.md` | 提取决策入 ADR，原件移出仓库 |
| 落地差异记录 | `旧: docs/implementation-notes.md` | 核对后移出；有效差异并入 `architecture/contracts.md` |
| 验收矩阵 / 验收快照 | `旧: docs/acceptance-matrix.md` | 保留「契约 → 守护测试」映射（去提交号）并入 `architecture/contracts.md` 附录，原件移出 |
| 逐批提交清单 / 里程碑流水 | `旧: docs/release-notes.md` §二、§三、§六 | 移出，转 `git tag` 注释与 GitHub Releases |
| 排障草稿 / 临时日志 | 当前无（历史排障结论已沉淀进 `known-gaps.md`） | 禁止入库，只留结论 |
| 会议记录 / 审查意见 | 当前无（审查结论已被 `review_regression_test` 固化） | 禁止入库，转 Issue 或测试用例 |

### 1.2 分类表（逐文件）

处理动作定义：**保留**＝原位或改名保留；**拆分**＝按内容性质拆到长期文档与过程归档两处；**迁移 ADR**＝决策内容改写为 ADR；**合并**＝并入其它文档后删除；**移出**＝`git rm` 出仓库，内容转团队知识库/GitHub Releases，历史由 git 保留。

| 文档 / 目录 | 类别 | 处理动作 | 去向 |
| --- | --- | --- | --- |
| `README.md` | 入口文档 | 保留（精简重写） | `README.md`，≤120 行，见 2.3 |
| `旧: docs/architecture.md` §1 架构决策 | 决策性 | 拆分 | `decisions/0002`–`0005`（决策表述）+ `architecture/overview.md`（决策摘要表） |
| `旧: docs/architecture.md` §2 模块边界 | 契约性 | 保留 | `docs/architecture/modules.md` |
| `旧: docs/architecture.md` §3–§6（所有权/线程/关闭、发布订阅、RPC、发现） | 契约性 | 保留 | `docs/architecture/contracts.md` |
| `旧: docs/architecture.md` §7 传输契约与兼容性 | 契约性 | 拆分 | `docs/architecture/contracts.md`（角色契约）+ `docs/architecture/wire-protocol.md`（帧格式） |
| `旧: docs/architecture.md` §8 评审与验证 | 过程记录 | 移出 | 验证入口改写进 `docs/guides/testing.md`，评审过程不入库 |
| `旧: docs/architecture.md` §9 已知差距 | 可追溯 | 保留 | `docs/architecture/known-gaps.md`（唯一限制清单） |
| `旧: docs/build.md` §2 target 拓扑、§2.2 开关、§2.3 注入接口、§2.4 测试分组、§3 行为不变量 | 契约性 | 拆分保留 | `docs/guides/build.md` |
| `旧: docs/build.md` §1 现状清单、§4 验收记录（T1–T6 + 提交号）、§6 批次收尾 | 过程记录 | 移出 | 团队 Wiki《Firefly 构建拆分过程》；有效自检项并入 `guides/build.md` §8 |
| `旧: docs/acceptance-matrix.md` 全表 | 过程记录 | 拆分 + 移出 | 「契约 → 守护测试」两列并入 `architecture/contracts.md` 附录 A（去掉批次号与提交号）；其余移出 |
| `旧: docs/release-notes.md` §一 里程碑结论 | 与 overview 重复 | 合并 | 有效结论并入 `architecture/overview.md`，原文删除 |
| `旧: docs/release-notes.md` §四 已知限制摘要 | 可追溯 | 合并 | `docs/architecture/known-gaps.md`（与 architecture §9 合并去重） |
| `旧: docs/release-notes.md` §五 兼容性声明 | 契约性 | 拆分保留 | `docs/architecture/wire-protocol.md` §「兼容性承诺」 |
| `旧: docs/release-notes.md` §二/§三/§六 逐批清单、产出文件、历史对应 | 过程记录 | 移出 | 团队 Wiki；发布历史保留在 `git tag` 注释与 GitHub Releases |
| `旧: docs/implementation.md` 全篇 | 过时设计稿 | 迁移 ADR + 移出 | 决策抽为 `decisions/0002`–`0005`；原文（含 GitHub 行号永久链接）移出 |
| `旧: docs/implementation-notes.md` 全篇 | 落地差异记录 | 核对 + 移出 | 有效契约并入 `architecture/contracts.md`；原文移出 |
| `旧: docs/new-backend-guide.md` | 操作指南 | 保留（改名） | `docs/guides/adding-backend.md` |
| `旧: docs/eval-zeromq-backend.md` | 决策性（未跟踪） | 迁移 ADR | `docs/decisions/0006-no-third-backend-zmq.md`（结论 + 成本区间），详细评估数据作为附录保留在同文件 |
| `.gitignore` 注释「第 4 批验证目录」 | 过程痕迹 | 就地修改 | 改为「无 NNG 验证目录（由 scripts/verify.sh 重建）」 |
| `include/firefly/node/writer.h` 等公共头的过程标记（`阶段一`、`本阶段`、`TODO(phase4)`、`nng PUB socket`） | 过程痕迹 | 按第三步改写 | 契约表述留头文件，理由链接 ADR |

### 1.3 混合文档的拆分口径（三条切分规则）

以 `旧: docs/build.md` 为例，切分时按以下顺序判断每段：

1. **读一年后仍要用来判断「能不能这么改」** → 长期文档（`guides/build.md`）。
2. **只在解释「当时为什么这样定」** → ADR（`decisions/`）。
3. **只记录「当时做到哪一步、谁验的、哪个提交」** → 移出仓库（git 已是权威记录，重复维护必然腐烂）。

> 硬性要求：拆分后的长期文档**不得出现**提交 hash、批次号、「第 N 批」「T1–T6」、审查人姓名。迁移时用 `grep -rnE --exclude=documentation.md '[0-9a-f]{7,40}|第 ?[0-9一二三四五] ?批|T[0-9]\b' docs/architecture docs/guides` 自查。
>
> `--exclude=documentation.md` 是必需的：本文作为规范必须引用这些反例词，不豁免会让自查与 CI 永远有输出。豁免范围仅限这一个文件，其它 `guides/*` 不得沿用此参数。

### 1.4 前置项结论

两项阻塞项已定论，迁移按此执行完毕：

| # | 事项 | 结论 |
| --- | --- | --- |
| 1 | 过程文档归档目的地 | **团队 Wiki**。首轮归档包（4 篇过程文档 + ZeroMQ 评估全文）已生成，上传后归档地址写入 `git tag` 注释 |
| 2 | 仓库内是否保留 `docs/history/` | **不保留**。理由：只读约束无法自动执行，实测会重新长成活跃文档；git 历史即过程事实的权威来源 |
| 3 | 离线查阅 API 文档的需求 | 无。`docs/api/` 只作 CI 产物，需要离线时随 Release 附加 zip |
| 4 | ZeroMQ 评估定论 | 已决定不做，[ADR-0006](../decisions/0006-no-third-backend-zmq.md) 状态 `Accepted（No-go）` |

仍待确认（不阻塞迁移，但影响第 5–7 步）：

| # | 事项 | 当前假设 | 影响 |
| --- | --- | --- | --- |
| 5 | 团队角色划分（架构/构建/协议/文档 owner） | 按角色定义，不写人名 | 5.1 表格的「负责人角色」列与评审分工 |
| 6 | Doxygen 版本 | 固定 1.9.8；`PROJECT_VERSION` 与 CMake 版本同步，不写死 | 本地与 CI 生成结果一致性 |

> 两项都只需在门禁接入 PR 之前确认。

---

## 第二步：目标目录结构

### 2.1 目录树

```text
Firefly/
├── README.md                        # 唯一入口，≤120 行
├── CONTRIBUTING.md                  # 3 行：指向 docs/guides/documentation.md + PR 模板
├── Doxyfile                         # 版本控制
├── docs/
│   ├── README.md                    # 文档索引（导航唯一入口，每篇文档必须在此登记）
│   ├── architecture/                # 「现在是什么」
│   │   ├── overview.md              # 定位、分层、决策摘要表、术语表
│   │   ├── modules.md               # 模块边界、依赖方向、target 与目录对应
│   │   ├── contracts.md             # 生命周期/并发/回调/错误码/送达语义 + 附录A 契约→测试
│   │   ├── wire-protocol.md         # FFRP 帧、发现文本协议、兼容性承诺
│   │   └── known-gaps.md            # 唯一限制清单（含未修 P2）
│   ├── decisions/                   # 「当时为什么」
│   │   ├── README.md                # ADR 索引表（编号/标题/状态/取代关系）
│   │   ├── 0000-template.md         # 模板，非 ADR；索引登记但不参与状态校验
│   │   ├── 0001-record-architecture-decisions.md
│   │   ├── 0002-split-transport-and-discovery-contracts.md
│   │   ├── 0003-four-role-transport-interfaces.md
│   │   ├── 0004-endpoint-record-discovery-model.md
│   │   ├── 0005-single-io-executor-per-client.md
│   │   ├── 0006-no-third-backend-zmq.md
│   │   └── 0007-document-taxonomy-and-process-exit.md
│   ├── guides/                      # 「怎么做」
│   │   ├── build.md                 # 依赖、target 拓扑、开关、命令、构建自检
│   │   ├── testing.md               # 测试分组、验收命令、跨进程互测、故障注入
│   │   ├── adding-backend.md        # 原 new-backend-guide.md
│   │   ├── releasing.md             # 版本号、tag 注释、Releases 正文模板
│   │   └── documentation.md         # 本文（规范自身）
│   └── api/                         # Doxygen 生成物，**不入库**（见 2.4）
├── include/firefly/                 # API 文档的唯一事实源（Doxygen INPUT）
└── .github/workflows/docs.yml       # Doxygen + 链接 + 孤儿文档检查
```

### 2.2 各目录职责边界

| 目录 | 存放 | **不存放** |
| --- | --- | --- |
| `README.md` | 一句话定位、能力表、最短上手路径、文档导航、License | 设计论证、变更历史、验收数据、完整 API 清单、ADR 内容 |
| `docs/README.md` | 全量文档索引 + 读者路径（新用户/改代码/发版） | 任何技术内容本身 |
| `architecture/` | 当前成立的行为边界、数据结构、线协议、已知限制；每条断言都能被代码或测试验证 | 提交号、批次号、任务分工、未采纳方案、性能口水话 |
| `decisions/` | 一次性决策的完整背景与代价；「不做某事」也在此 | 现状描述（现状归 architecture，ADR 只写「因此当前如何」一句并链接）、操作步骤 |
| `guides/` | 可照做的操作序列、命令、排错分支、验收判据 | 设计理由（链接 ADR）、版本流水 |
| `docs/api/` | 生成产物（HTML/XML） | 手工编辑的任何内容 |

**命名约定**：`architecture/` 与 `guides/` 一律小写短横线、名词短语、一文件一主题；禁止 `notes`、`misc`、`tmp`、`draft`、`v2`、`final`、`old` 这类词出现在文件名中（CI 检查）。

### 2.3 根 `README.md` 的定位与长度约束

- **定位**：给第一次打开仓库的人 5 分钟内跑起来，并把他送到正确的文档。它是**索引**，不承担任何论证。
- **长度硬约束**：≤120 行（约两屏）。超出即判定为「内容放错位置」，必须移到 `docs/`。
- **必须包含**：一句话是什么、能力表（4 行以内）、Linux/Windows 各一段最短构建命令、文档导航（4 个链接：架构、构建、后端接入、ADR 索引）、License。
- **必须不包含**：架构原理、重构历史、验收数字、`beta/实验性` 之类的临时状态说明、超过 5 个的代码块。
- **CI 校验**：`wc -l README.md` > 120 即失败（见 6.3）。

### 2.4 `docs/api/` 是否纳入版本控制

**结论：不纳入。** 理由与例外：

| 维度 | 判断 |
| --- | --- |
| 可重建性 | 由 `include/**` + 固定版本 Doxygen 完全确定，入库属重复存储 |
| Diff 噪声 | 生成物每次改动数百个 HTML 文件，淹没真实改动，且 review 无法逐行看 |
| 体量 | 单次生成约 5–15 MB，随历史累积不可回收（git 不擅长删除大目录） |
| 发布方式 | CI 生成 → 上传 GitHub Pages（`gh-pages` 分支或 artifact），仓库主分支不含 |
| 处置 | `.gitignore` 增加 `docs/api/`；CI 增加「`git ls-files docs/api` 必须为空」检查 |
| 唯一例外 | 需要离线交付时，把生成物作为 **Release asset（zip）** 附加，仍不进 git |

---

## 第三步：Doxygen 注释规范

### 3.1 形式与生效条件

- 公共头（`include/firefly/**`）的公开接口**必须**使用 Doxygen 块注释：`/** ... */` 或 `///` 三斜线。**`//` 双斜线注释不会被 Doxygen 解析**——用 `// @post`、`// @threadsafe` 写的“标签”在 HTML 里会整体消失，这是注释失效最常见的原因。
- 注释语言：中文正文；标识符、命令行、类型名保持英文。
- 注释位置：**声明处**（头文件），不在 `src/` 重复。实现文件只允许写实现理由，且不得使用 `@brief` 等文档标签。
- 每个公共头文件首行必须有 `@file`：

```cpp
/// @file writer.h
/// @brief 发布端点 Writer<MessageT> 的声明与发布送达语义。
```

### 3.2 类、公开函数、枚举、宏分别怎么写

| 对象 | 必填标签 | 可选标签 | 硬性要求 |
| --- | --- | --- | --- |
| 类 / 模板类 | `@brief`（一句话，做什么）；模板参数逐个 `@tparam` | `@note`（线程安全、生命周期）、`@warning` | 必须写清「谁拥有它、怎么销毁」；`@brief` 不得以「这是一个…」开头 |
| 公开函数 / 方法 | `@brief`；每个参数 `@param[in]`/`@param[out]`；有返回值必写 `@return` | `@retval`（具名码/枚举值）、`@pre`、`@post`、`@note`、`@warning`、`@exception`、`@sa`、`@deprecated`、`@todo` | 参数名必须与签名完全一致；`@return` 不得只写「返回结果」；失败语义必须写清哪个码对应哪种路径 |
| 枚举 / 枚举值 | 枚举类型 `@brief` 说明整体语义与使用约束 | — | 每个枚举值**逐个**用行尾 `///< 说明`；说明必须写「何时出现」，而非重复值名字面意思 |
| 宏 / 常量 | `/// @brief` 一句 | `@note`（能否多次展开、是否线程安全、单位） | 常量必须写单位与取值范围；宏必须说明求值次数与副作用 |

补充规则：

- 公开函数**不抛异常**时不写 `@exception`；会抛（含 `std::future::get()` 透出 `FireflyException`）必须列出类型与触发条件。
- 状态机类接口（`GetReader` → `Shutdown` → 再调用）必须用 `@pre`/`@post` 表达前后置条件，不允许写成大段散文。
- 线程安全声明统一用 `@par 线程安全` 段落（Doxygen 内置 `@par`），**不要**自定义 `@threadsafe`；若团队坚持自定义短标签，必须在 `Doxyfile` 的 `ALIASES` 中定义，否则生成结果会静默丢失。

### 3.3 标签风格（白名单）

| 用途 | 统一写法 | 禁止写法 |
| --- | --- | --- |
| 命令前缀 | `@`（`@brief`） | `\brief`（不混用） |
| 简述 | `@brief` | `@summary`、`@short` |
| 参数 | `@param[in] name` / `@param[out]` / `@param[in,out]` | 裸 `@param name`（方向不明） |
| 返回值 | `@return` + 具名值用 `@retval` | `@returns`、`@ret` |
| 异常 | `@exception Type 条件` | `@throws`（统一取 `@exception`） |
| 废弃 | `@deprecated 替代方案 + 生效版本` | 仅写「已废弃」不带替代 |
| 交叉引用 | `@sa`（指向 ADR 或同级文档路径） | 指向 GitHub 具体 commit 的行号 URL |
| 待办 | 公共头一律 `@todo` + 纯技术描述（缺什么、补齐后应满足什么） | `TODO(phase4)`、`TODO(@某人)`、`FIXME(#123)`，即任何带阶段号/人名/单号的大写裸标记 |
| 正文详述 | 直接写段落，`@brief` 后空一行 | `@details`（本项目禁用，避免双份摘要） |

> `TODO` 的禁用边界要说清楚，否则门禁会误杀：**公共头里不允许任何裸 `TODO(`**（因为公共头只该有 `@todo`）；`src/` 实现文件允许 `TODO(<纯技术描述>)`，例如 `TODO(重连退避还需抖动)`，但禁止 `TODO(phase4)`、`TODO(@某人)`、`FIXME(#123)`。CI 因此只匹配 `TODO\(phase`、`TODO\(@`、`FIXME\(#` 三种确定违规的形式，**不**匹配裸 `TODO\(`。

### 3.4 注释中禁止出现的内容

| 禁止项 | 典型形态 | 为什么 |
| --- | --- | --- |
| Issue / PR 号 | `FIXME(#123)` | 仓库迁移或 issue 关闭后成为死引用 |
| 提交 hash / 批次号 / 阶段号 | `writer.h`：`阶段一`、`阶段三（任务 2）`、`本阶段不实现`；`build.md`：`第 4 批` | 对读者无意义，且随重构立刻失真 |
| 审查人 / 作者 / 日期 | — | 归属由 git blame 回答，写进注释只会过期 |
| 临时版本标记 | `writer.h`：`TODO(phase4)`；`types.h`：`保留一个版本供二进制消费方灰度` | 读者无法判断它是否仍然成立 |
| 后端实现细节泄漏 | `writer.h`：`nng PUB socket 的发送队列`、`nng 返回 EAGAIN` | 违反「公共头不暴露后端」的分层契约 |
| 失效的永久链接 | `implementation.md` 中 `github.com/.../blob/<hash>/firefly/...#L23-L52` | 目录已从 `firefly/` 迁到 `src/`，链接必然 404 |
| 营销词与主观评价 | 「强大」「优雅」「性能极佳」 | 不可验证 |

**替代做法**：需要解释「为什么现在这样」时，写一句现状 + `@sa docs/decisions/0003-four-role-transport-interfaces.md`；需要标记缺口时，写 `@todo 说明缺什么`，把编号留在 Issue。

### 3.5 正例

**正例 1：公开函数（含送达语义、线程安全、前置条件）**

```cpp
/**
 * @brief 向本通道发布一条消息。
 *
 * kAccepted 仅表示消息已交给本地传输层，**不表示对端已收到**。
 *
 * @param[in] msg 待发布消息；内部按值拷贝一次。
 * @return 本次写入的受理结果。
 * @retval kAccepted           已交给本地传输层，不构成送达承诺。
 * @retval kNoSubscriber       通道当前无 Reader，消息未发送。
 * @retval kQueueFull          本地发送队列已满，消息未发送。
 * @retval kSerializationError 序列化失败，消息未发送。
 * @retval kTransportError     Writer 未初始化、已关停，或底层传输失败。
 *
 * @pre Writer 已成功 Init()。
 * @post 失败时 Writer 仍可用，调用方可直接重试。
 *
 * @par 线程安全
 * 可与同一 Writer 的其他 Write 并发；不得与 Shutdown() 并发。
 *
 * @note 同一 Writer 的多次 Write 按调用顺序进入本地队列；跨 Writer、跨进程、
 *       跨订阅者均不保证到达顺序，需要顺序时由业务层携带序号重排。
 * @sa docs/architecture/contracts.md 「发布送达语义」一节
 */
WriteStatus Write(const MessageT& msg);
```

**正例 2：枚举（逐值行尾注释 + 废弃标注）**

```cpp
/**
 * @brief 端点操作的统一错误码。
 *
 * 每个码都对应一条可触发的失败路径；禁止用 bool 或空指针折叠失败原因。
 */
enum class ErrorCode {
  kOk = 0,            ///< 成功。
  kInvalidName,       ///< 名称非法：为空、含非法字符，或命中服务名保留后缀。
  kDuplicate,         ///< 同进程内已存在同名端点（仅创建路径查重）。
  kSerialization,     ///< 序列化或反序列化失败。
  kTimeout,           ///< 在给定截止时间前未等到响应。
  kCancelled,         ///< 调用方显式撤销，请求未在服务端执行。
};

/**
 * @brief 旧版二值错误码，仅为二进制兼容保留。
 * @deprecated 自 0.5 起废弃，改用 ErrorCode 配合 Result<T>；两者不得混用。
 */
enum [[deprecated("use ErrorCode with Result<T>")]] ReturnCode {
  SUCC = 0,  ///< 成功（等价于 ErrorCode::kOk）。
  FAIL = 1,  ///< 失败（无法区分原因，不要在新代码中使用）。
};
```

**正例 3：模板类 + 会抛异常的异步接口**

```cpp
/**
 * @brief 请求方端点：向同名 Service 发起请求应答调用。
 *
 * @tparam RequestT  请求消息类型，须为 google::protobuf::Message 派生类。
 * @tparam ResponseT 响应消息类型，同上。
 *
 * @warning 本类由 Node 创建并返回 shared_ptr；Node 析构不销毁端点，
 *          须由持有者调用 Node::DeleteClient() 或释放最后一个引用。
 * @sa docs/decisions/0005-single-io-executor-per-client.md
 */
template <typename RequestT, typename ResponseT>
class Client : public ClientBase {
 public:
  /**
   * @brief 异步发起一次请求。
   *
   * @param[in] request 请求消息；内部拷贝。
   * @param[in] timeout 相对超时，从入队时刻起算。
   * @return future：成功时兑现 Response，失败时由 get() 抛出异常。
   * @exception FireflyException get() 在超时（kTimeout）、队列满（kQueueFull）
   *            或已取消（kCancelled）时抛出，message 含定位线索。
   * @note 返回的 future 保证在截止时间内兑现，不存在永不完成的情况。
   */
  std::future<ResponseT> AsyncSendRequest(const RequestT& request,
                                          std::chrono::milliseconds timeout);
};
```

### 3.6 反例（注释改造前的本仓库形态，保留作对照）

**反例 1：用 `//` 写 Doxygen 标签，且标签非法**

```cpp
// 反例（改造前的 writer.h）
// @post 调用后本 Writer 不可再用；后续 Write 返回 kTransportError
// （WriteStatus 无 kNotReady 码，按阶段一契约折叠为传输错误）、HasReader 返回 false。
void Shutdown() override;
```

问题：① `//` 不是文档注释，Doxygen 完全忽略；② 引用了不存在的「阶段一契约」；③ 用散文描述状态机，未用 `@post`。改法：`/// @post 调用后本 Writer 不可再用；Write 返回 kTransportError，HasReader 返回 false。`

**反例 2：公共头泄漏后端实现 + 过程标记**

```cpp
// 反例（改造前的 writer.h 顶部契约块，节选）
//   kAccepted          消息已成功交给**本地传输层**（nng PUB socket 的发送队列）。
//   kReliable  ：**本阶段与 kBestEffort 行为完全相同**。nng PUB/SUB 没有 ack…
//                TODO(phase4): 传输层可靠性增强后在此处更新契约
```

问题：① 公开 API 的契约写死在 `nng` 上，换后端即失真，也直接违反「公共头不含后端」的分层约束；② `本阶段`、`TODO(phase4)` 是过程标记，读者无法判断是否已实现；③ 30 行大段散文替代了本应写在 `contracts.md` 的语义说明。

**反例 3：枚举值注释不可被工具抓取，且术语未定义**

```cpp
// 反例（改造前的 types.h）
enum class ErrorCode {
  kOk = 0,
  kInvalidName,        // 通道/节点/服务名非法（空、含非法字符、命中服务后缀保留字）
  kDuplicate,          // 名称重复（本阶段仅 Reader 查重落此码）
};
```

问题：① `//` 紧跟值后不是 `///<`，Doxygen 不抓取，API 页面里枚举值是空的；② 「服务后缀保留字」是内部术语，未在 `overview.md` 术语表定义；③ 「本阶段仅…」是过程信息，应删除（现状语义已由 `contracts.md` 固化）。

### 3.7 `Doxyfile` 关键配置（必须显式设置）

```ini
PROJECT_NAME           = "Firefly"
OUTPUT_DIRECTORY       = docs/api
INPUT                  = include/firefly
RECURSIVE              = YES
FILE_PATTERNS          = *.h
EXTRACT_ALL            = NO
WARN_IF_UNDOCUMENTED   = YES     # 仅对 INPUT 白名单生效，不检查 src/
WARN_AS_ERROR          = YES     # 文档告警即 CI 失败
WARN_NO_PARAMDOC       = YES     # 有 @param 缺说明即告警
JAVADOC_AUTOBRIEF      = NO      # 强制显式 @brief，避免首行被当摘要
QT_AUTOBRIEF           = NO
GENERATE_LATEX         = NO
GENERATE_TREEVIEW      = YES
EXCLUDE_PATTERNS       = */detail/*
HTML_OUTPUT            = html
```

> `EXCLUDE_PATTERNS = */detail/*`：`include/firefly/detail/` 是类型擦除实现细节，不属于公开 API，不纳入强制注释范围（但其声明仍需按 3.2 写，只是不参与 `WARN_AS_ERROR`）。

---

## 第四步：ADR 模板与使用规则

### 4.1 命名与索引

- 文件名：`docs/decisions/NNNN-kebab-case-english-title.md`，四位数字、**单调递增、永不复用、永不重排**；标题英文短横线（保证跨平台排序与工具兼容），正文中文。
- 编号一旦分配即永久绑定该决策；作废的 ADR 保留文件与编号，只改状态。
- 索引：`docs/decisions/README.md` 必须含全量表格，任一 ADR 文件未登记即 CI 失败。
- `0000-template.md` 也必须在索引表中登记，状态列写「—」、标题写「模板（非 ADR）」，**不参与**状态校验与取代关系校验；完整示例见附录 A.3。

索引表格式（示例）：

| 编号 | 标题 | 状态 | 日期 | 取代关系 |
| --- | --- | --- | --- | --- |
| 0000 | 模板（非 ADR） | — | — | 不参与状态校验 |
| 0001 | 用 ADR 记录架构决策 | Accepted | 2026-11-24 | — |
| 0003 | 传输按四角色建模 | Accepted | 2026-11-24 | 取代 0002 中 `ISocket` 部分 |

### 4.2 模板（`docs/decisions/0000-template.md`）

```markdown
# ADR-0007: 文档分层与过程记录出库

- **状态**：Proposed | Accepted | Superseded by ADR-XXXX | Deprecated
- **日期**：2026-11-24
- **决策者**：架构 owner、文档 owner（写角色，不写人名）
- **关联代码**：`include/firefly/node/writer.h`、`docs/guides/documentation.md`
- **取代**：无 / 取代 ADR-0003

## 背景

只写约束与事实：当前哪里有问题、有哪些不可回避的限制（兼容性、依赖、人力、上游协议）。
不写解决方案，不写「我们讨论了三轮」。

## 决策

一句话陈述结论（「决定 …」），随后用 3–6 条说明边界：适用范围、明确不做什么、
必须维持的不变量。此节必须能被未来的读者直接照做判断。

## 后果

- **正面**：获得什么、消除了什么风险。
- **负面/成本**：付出什么（人力、性能、维护面翻倍、构建变慢），必须写具体量级。
- **中性**：改变了哪些惯例，需要谁调整习惯。
- **如何验证仍被遵守**：指名测试用例、编译期检查或 CI 规则（如 `no_leak_check`）。

## 备选方案

| 方案 | 未采纳原因 |
| --- | --- |

## 修订记录

| 日期 | 变更 | 原因 |
| --- | --- | --- |
```

### 4.3 何时新增、何时不新增

**必须新增**（满足任一）：

1. 不可逆或回退成本高的技术选择（换后端、换线协议、换并发模型）。
2. 跨模块契约变更（公共 API 签名、错误码集合、回调线程语义、帧格式）。
3. 引入或移除第三方依赖、改变构建目标拓扑。
4. 兼容性承诺（是否保证旧版二进制互通、废弃窗口多长）。
5. **决定不做某事**且预期会被反复提议（如 ADR-0006 不引入 ZeroMQ）——这类 ADR 的收益最高。
6. 推翻既有决策（新 ADR 取代旧 ADR，两者都保留）。

**不得新增**：重命名、bug 修复、实现内部细节、可逆的小改动、纯文档措辞调整、排期与人力安排（那是项目管理，不是架构决策）。

### 4.4 「被取代」的处理，以及为什么不能直接改写历史

- 被取代时，**只改两处**：旧 ADR 的状态行改为 `Superseded by ADR-0012`，并在文件首行下加一行横幅 `> 已被 ADR-0012 取代（链接到 0012-xxx.md），勿据此实现。`；新 ADR 头部写 `取代：ADR-0003`，正文「背景」说明原决策为何失效。
- **禁止**删除旧 ADR、禁止把旧结论改写为新结论、禁止复用编号。
- 原因有三：① ADR 的价值是**决策轨迹**，读者需要知道「当年为什么没选 X」，改写会永久丢失这个信息；② 约束条件会变，若改写历史，后人会误以为今天的方案在当时就是显然的，从而低估迁移成本；③ 同一编号出现两种结论会让引用（代码 `@sa`、Issue、Wiki）指向歧义内容。
- 唯一允许的编辑是**勘误**（错别字、失效链接、格式），且必须在「修订记录」表留一行，注明未改变结论。

### 4.5 首批 ADR 清单（从现有文档抽取）

| 编号 | 来源 | 状态 |
| --- | --- | --- |
| 0001 用 ADR 记录架构决策 | 本文 | Accepted |
| 0002 传输与发现拆为两个可替换契约 | `旧: docs/implementation.md` §分层与职责 | Accepted |
| 0003 传输按四角色建模，取代通用 ISocket | `旧: docs/implementation.md` §四角色 + `architecture.md` §7 | Accepted |
| 0004 发现层采用 EndpointRecord 记录模型 | `旧: docs/implementation.md` §发现层 + `implementation-notes.md` | Accepted |
| 0005 每 Client 单 I/O 执行者、Reader/Service 串行回调 | `旧: docs/architecture.md` §1、§5 | Accepted |
| 0006 不引入 ZeroMQ 第三后端 | `旧: docs/eval-zeromq-backend.md` | Accepted（No-go） |
| 0007 文档分层与过程记录出库 | 本文 | Proposed |

---

## 第五步：文档维护规则表

### 5.1 触发条件 → 更新时机 → 负责人

| 文档位置 | 维护触发条件 | 同一 PR 内更新 | 负责人角色 |
| --- | --- | --- | --- |
| `README.md` | 新增/删除公开入口；构建前置或最短命令变化；能力表所列能力增减 | **是** | PR 作者 |
| `docs/README.md` | 任意文档新增/改名/删除/合并 | **是** | PR 作者（文档 owner 复核） |
| `docs/architecture/overview.md` | 分层或模块职责变化；新增术语 | **是** | 架构 owner |
| `docs/architecture/modules.md` | 目录搬迁、include 根变化、target 增减、依赖方向变化 | **是** | 构建 owner |
| `docs/architecture/contracts.md` | 生命周期、取消、回调线程、错误码集合、送达语义、QoS 语义任一变化 | **是**（行为变更 PR 的合并前置条件） | 模块 owner + 架构 owner 双签 |
| `docs/architecture/wire-protocol.md` | FFRP 帧字段、发现文本命令、版本协商、兼容窗口变化 | **是**（且必须先有 ADR） | 协议 owner |
| `docs/architecture/known-gaps.md` | 修掉一条限制 / 发现新的限制 | **是** | 修复者 |
| `docs/decisions/*.md` | 见 4.3 触发清单 | **是**（决策 PR 本身） | 决策提出者 + 架构 owner |
| `docs/guides/build.md` | CMake 选项、target 拓扑、依赖版本、验证命令变化 | **是** | 构建 owner |
| `docs/guides/testing.md` | 测试分组、新增套件、验收命令、故障注入方式变化 | **是** | 测试 owner |
| `docs/guides/adding-backend.md` | 四角色接口、`IRegistry`、注入入口签名变化 | **是** | Core owner |
| `docs/guides/releasing.md` | 版本策略、tag 规范、Releases 模板变化 | **是** | 发布 owner |
| `docs/guides/documentation.md` | 规范自身调整 | 独立 PR，不与代码混提 | 文档 owner |
| `include/firefly/**/*.h` 的 Doxygen | 公开签名、语义、线程安全、异常行为变化 | **是**（Doxygen 门禁强制） | PR 作者，评审者必查 |
| `docs/api/` | **不手工维护** | 不适用 | CI 生成，无人负责 |

「同一 PR 内更新」的含义：文档与代码必须在**同一个提交序列**里合并，不允许「先合代码，文档下个 PR 补」。理由很实际——补文档的 PR 永远排在业务需求之后，一周之后作者上下文已丢。

### 5.2 「一事一处」原则

**规则**：一个事实只在一篇文档里写全，其它位置只允许**一句话摘要 + 链接**。

| 事实 | 唯一权威位置 | 其它位置怎么写 |
| --- | --- | --- |
| `Write` 的送达语义（`kAccepted` 不等于送达） | `architecture/contracts.md` | `writer.h`：`@brief` 一句 + `@sa`；`README.md`：不写 |
| 错误码全集与触发路径 | `architecture/contracts.md` | `types.h`：逐值 `///<` 一句；不重复整表 |
| FFRP 帧布局 | `architecture/wire-protocol.md` | `rpc_frame.h`：`@sa` |
| 构建 target 拓扑与开关 | `guides/build.md` | `architecture/modules.md`：一张关系图 + 链接 |
| 已知限制与未修 P2 | `architecture/known-gaps.md` | `guides/*` 中不重复列举，只链接 |
| 某决策的理由与代价 | 对应 `decisions/NNNN-*.md` | `architecture/*`：`@sa` 或行内链接 |

**反例（整理前的本仓库）**：`kAccepted` 的送达语义同时写在 `writer.h` 的 30 行注释、`architecture.md` §4 与发布说明里，三处措辞已经不一致。现在只在 `contracts.md` 写全，其余位置一句话加链接。

**判定方法**：如果同一个事实在两处以上出现完整描述，就说明其中一处必然过期；CI 无法检测，靠评审时问一句「这条事实的主文档是哪篇？」

---

## 第六步：落地执行顺序与检查清单

### 6.1 七步落地顺序（按依赖排序，可分阶段合并）

| 步骤 | 内容 | 完成判据 | 预估工作量 |
| --- | --- | --- | --- |
| **1. 建骨架 + 冻结** | 建 `docs/{architecture,decisions,guides}`；写 `docs/README.md` 索引（先登记现存全部文档，标注「待迁移」）；`docs/api/` 加入 `.gitignore`；提交 `Doxyfile` 并本地跑通一次生成 | `doxygen Doxyfile` 退出码 0；索引覆盖 100% 现存文档 | 0.5 天 |
| **2. 过程文档出库** | 先把 `acceptance-matrix` / `implementation` / `implementation-notes` 原文导出到团队知识库；再 `git rm`；在 tag `refactor-complete` 上加注释指向归档地址 | 仓库内不再有批次号/验收快照类文件；归档链接可访问 | 0.5 天 |
| **3. 拆分混合文档** | `build.md` → `guides/build.md`（只留契约与命令）；`release-notes.md` → 兼容性声明入 `wire-protocol.md`、限制入 `known-gaps.md`、其余出库；`architecture.md` → 四个文件 | 每篇长期文档通过 1.3 的正则自查（无 hash / 无批次号） | 1–1.5 天 |
| **4. 抽取 ADR** | 按 4.5 清单写 0001–0007；`decisions/README.md` 索引 | 索引与文件一一对应；每篇含「后果」与「如何验证」 | 1 天 |
| **5. 公共头注释改造** | 先改 5 个核心头（`writer.h`、`reader.h`、`client.h`、`service.h`、`node.h`），清除后端泄漏与过程标记；再补 `types.h` 枚举与 `channel_config.h` | `doxygen` 零 warning；`grep -r '@threadsafe\|阶段[一二三四五]\|TODO(phase' include/` 无输出 | 1.5–2 天 |
| **6. 门禁接入** | 加 `docs.yml`（Doxygen / 链接 / 孤儿 / README 行数 / ADR 索引 / api 未入库）；更新 PR 模板 | 故意制造一次文档缺陷，CI 必须红 | 0.5 天 |
| **7. 首轮审计** | 跑 6.4 审计脚本；修掉孤儿文档与死链；把「文档检查」写入团队评审清单 | 审计输出为空；评审模板已生效 | 0.5 天 |

> 顺序不可交换的关键点：**必须先执行第 2 步再执行第 3 步**。否则拆分时会被迫在过程文档里继续搬运提交号，等于新建了一份过程文档。

### 6.2 PR 模板中的文档检查项

```markdown
## 文档检查（勾选或说明不适用）
- [ ] 公开接口（`include/firefly/**`）签名或语义变化时，Doxygen 注释已同步，本地 `doxygen Doxyfile` 无 warning
- [ ] 行为/生命周期/错误码/送达语义变化时，`docs/architecture/contracts.md` 已同步（同一 PR）
- [ ] 线协议或发现文本协议变化时，先有 ADR，且 `wire-protocol.md` 已更新
- [ ] 操作步骤变化（构建命令、CMake 选项、验证入口）时，`docs/guides/*` 已同步
- [ ] 目录/target/依赖变化时，`modules.md` 与 `build.md` 已同步
- [ ] 新增限制或修复限制时，`known-gaps.md` 已更新
- [ ] 本次为不可逆/跨模块决策，已新增 ADR（编号已登记到 `decisions/README.md`）
- [ ] 新增文档已登记到 `docs/README.md`；无重复描述同一事实的第三处
- [ ] 无过程信息入库：无提交 hash、批次号、Issue/PR 号、审查人、临时版本标记
- [ ] `README.md` 仍 ≤120 行；未手工修改 `docs/api/`
```

### 6.3 CI 检查（`.github/workflows/docs.yml`）

除 Doxygen 外的全部检查由 `scripts/check_docs.py` 一项实现，本地与 CI 跑同一条命令，避免两套逻辑漂移。

| 检查 | 实现 | 失败条件 | 必需性 |
| --- | --- | --- | --- |
| Doxygen 生成 | `doxygen Doxyfile`（`WARN_AS_ERROR=YES`） | 任一 warning：未文档化公开符号、参数名不匹配、`@param` 缺说明 | **必需** |
| 站内链接与锚点 | `scripts/check_docs.py` | 相对链接指向不存在的路径，或 `#锚点` 在该文件中无对应标题 | **必需** |
| 孤儿文档 | 同上：`docs/**/*.md` 每个文件必须被 `docs/README.md`、`decisions/README.md` 或其它文档以相对路径引用；两个索引页与 `0000-template.md` 免检（见注 2） | 存在无入链文件 | **必需** |
| ADR 索引一致性 | 同上：`decisions/[0-9]{4}-*.md` 与 `decisions/README.md` 表格双向比对；排除 `0000-template.md`；校验链接目标文件名、状态取值、以及被取代的 ADR 是否存在 | 文件未登记 / 登记了不存在的文件 / 链接名不符 / 状态非法 | **必需** |
| 失效标签扫描 | 同上：模式 `@threadsafe`、`阶段[一二三四五]`、`第 ?[0-9] ?批`、`TODO(phase`、`TODO(@`、`FIXME(#`、`@version`，扫描 `docs/**` 与 `include/**` | 有匹配（规范文档自身豁免，见注 1） | **必需** |
| `docs/api` 未入库 | 同上：`git ls-files docs/api` 必须为空 | 有跟踪文件 | **必需** |
| `docs/history` 不存在 | 同上 | 目录存在（过程记录一律归档到团队 Wiki） | **必需** |
| README 行数 | 同上：`README.md` ≤120 行 | 超限 | **必需** |
| 禁用词文件名 | 同上：文件名匹配 `notes\|misc\|tmp\|draft\|v2\|final\|old` | 有匹配（`--strict` 时升级为失败，CI 使用严格模式） | 建议 |
| 外链巡检 | `scripts/check_docs.py --external`，每周一定时执行 | 不阻断 PR，只在运行日志告警 | 建议 |

**注 1（规范文档豁免）**：`docs/guides/documentation.md` 为说明规则必须引用反例词（3.3 表格的 `@threadsafe`、3.6 的 `阶段一`/`TODO(phase4)`），不豁免则首次跑 CI 必然失败。脚本对该文件**整文件豁免**；若日后该文件变长、担心豁免掩盖真实违规，改用标记区间豁免：

```markdown
<!-- docs-check:off -->
（反例内容）
<!-- docs-check:on -->
```

整文件豁免只允许用于本文件，**任何其它 `guides/*` 不得使用**（评审时按此判定）。

**注 2（`0000-template.md` 的两种处理）**：二选一，不要两种混用。

- 方案 A（推荐，已采用）：索引表登记一行 `| 0000 | 模板（非 ADR） | — | — | — |`，脚本排除该文件的状态校验，仅校验「文件存在且被索引引用」。
- 方案 B：索引完全不登记，脚本以 `0000-*` 前缀硬排除。

采用 A 的理由：新人打开索引就能找到模板入口；B 会让模板只能靠 `4.2` 正文的路径发现，孤儿检查一旦误报就要临时改脚本。

改脚本本身的门槛：新增检查项必须同时进本文 6.3 表格与 PR 模板，否则等于新增了一条没人知道的规则。

### 6.4 季度审计（人工，30 分钟）

1. `known-gaps.md` 逐条核对：已修但未删的、新增但未记的。
2. 随机抽 3 篇 `architecture/*`，验证文中每条断言仍能被代码或测试证实。
3. 检查是否有 ADR 实际已被推翻但状态仍为 `Accepted`。
4. 检查 `guides/*` 中所有命令能否原样跑通（这是最容易腐烂的部分）。
5. 统计 `docs/` 总行数趋势：若持续增长而内容质量未升，说明又在收集过程文档。

---

## 附录 A：迁移执行记录（2026-11-24 已完成）

> 本节记录首轮迁移的**方法与顺序**，供同类整理参考；它不是日常流程，也不应在后续 PR 中重复执行。
> 首轮结果：旧 8 篇文档 + 1 篇未跟踪评估稿 → `architecture/` 5 篇、`guides/` 5 篇、`decisions/` 9 个文件；4 篇过程文档与评估全文归档到团队 Wiki（归档包在 `.workbuddy/wiki-export/2026-11-24-process-docs/`，上传后可删）。
> 第 5–7 步（公共头注释改造、CI 门禁、首轮审计）仍未执行。

> 迁移只机械处理建目录、改名、复制、删除、索引占位；拆分后的正文、ADR 正文、`testing.md` / `releasing.md` 的内容均已人工撰写（清单见 A.2）。仅按 A.1 命令跑完只会得到「结构正确、内容待填」的骨架。
>
> 执行前置：A.1 第 4 步依赖 1.4 的待确认项 1、2 有定论；第 1–3 步与 5 步无前置依赖。

### A.1 机械操作

```bash
# 0) 确认工作区：应只有未跟踪的 docs/eval-zeromq-backend.md
git status --short

# 1) 建目录、占位索引、忽略生成物
mkdir -p docs/architecture docs/decisions docs/guides
printf 'docs/api/\n' >> .gitignore
printf '# Firefly 文档索引\n\n（按附录 A.3 模板补全）\n' > docs/README.md
printf '# 架构决策记录（ADR）索引\n\n（按附录 A.3 模板补全）\n' > docs/decisions/README.md

# 2) 纯改名（无内容拆分）
git mv docs/new-backend-guide.md docs/guides/adding-backend.md

# 3) 拆分类：先复制，按 A.2 裁剪完并自查通过后再删原件
cp docs/architecture.md docs/architecture/overview.md
cp docs/architecture.md docs/architecture/modules.md
cp docs/architecture.md docs/architecture/contracts.md
cp docs/build.md        docs/guides/build.md
#   ↑ 三份 architecture/* 与 build.md 裁剪完成后：
#   git rm docs/architecture.md docs/build.md

# 4) 过程文档出库（确认待确认项 1、2 后再执行；先导出到知识库）
git rm docs/acceptance-matrix.md docs/implementation.md docs/implementation-notes.md
git rm docs/release-notes.md

# 5) 未跟踪的评估稿转 ADR（未跟踪文件用 mv，不用 git mv）
mv docs/eval-zeromq-backend.md docs/decisions/0006-no-third-backend-zmq.md
#   随后按 4.2 模板补齐元数据、「后果」与「备选方案」章节

# 6) 自查：长期文档中不得残留过程标记（规范文档自身豁免）
grep -rnE --exclude=documentation.md \
  '[0-9a-f]{7,40}|第 ?[0-9一二三四五] ?批|T[1-9]\b|阶段[一二三四五]' \
  docs/architecture docs/guides
```

### A.2 需人工撰写的文件（A.1 不生成内容）

| 文件 | 来源 | 撰写口径 | 对应步骤 |
| --- | --- | --- | --- |
| `architecture/overview.md` | `cp` 自 `旧: docs/architecture.md` | 保留 §1 决策摘要表（每条改为「结论 + ADR 链接」）、§2 分层图、新增术语表；删除 §3–§9 | 3 |
| `architecture/modules.md` | `cp` 同上 | 只保留 §2 模块边界表与依赖约束，补「target ↔ 源码目录」对应列 | 3 |
| `architecture/contracts.md` | `cp` 同上 | 保留 §3–§7（所有权/线程/关闭、发布订阅、RPC、发现、传输契约）；新增附录 A「契约 → 守护测试」（从 `acceptance-matrix` 去掉批次号与提交号后平移） | 3 |
| `guides/build.md` | `cp` 自 `旧: docs/build.md` | 保留 §2（target 拓扑、开关、注入接口、测试分组）、§3 行为不变量、§6 验证命令；删除 §1 现状清单、§4 验收记录、§5 批次自检 | 3 |
| `architecture/wire-protocol.md` | **新建** | FFRP 帧布局（事实源 `src/core/rpc_frame.h`）、发现层文本命令与选举、`旧: release-notes` §五 兼容性承诺 | 3 |
| `architecture/known-gaps.md` | **新建** | `旧: architecture.md` §9 与 `旧: release-notes.md` §四 去重合并为唯一清单 | 3 |
| `decisions/0000-template.md` | **新建** | 直接复制 4.2 的模板代码块 | 4 |
| `decisions/0001`–`0005`、`0007` | **新建** | 按 4.2 模板，来源见 4.5 表 | 4 |
| `decisions/0006-no-third-backend-zmq.md` | `mv` 自 `旧: docs/eval-zeromq-backend.md` | 补 4.2 元数据与「后果」；原评估数据降为附录章节 | 4 |
| `guides/testing.md` | **新建** | 测试分组（contract/integration/support）、`ctest --preset default`、跨进程互测屏障与端口、`scripts/verify.sh` 用法与已知抖动 | 5 |
| `guides/releasing.md` | **新建** | 版本号规则、annotated tag 规范、GitHub Releases 正文模板、兼容性核对项 | 5 |
| `Doxyfile` | **新建** | 直接使用 3.7 的配置 | 5 |

### A.3 两个索引文件的最小模板

> 过渡期用法：若在第 1 步就建索引（此时文档尚未迁移），先按现存的 8 篇登记、每行标「待迁移」，保证「所有文档有入链」这条从第一天就成立；A.2 全部完成后替换为下面的目标版本。

`docs/README.md`：

```markdown
# Firefly 文档索引

> 文档的唯一入口。新增、改名、删除文档必须同步本表，否则 CI 的孤儿检查会失败。

## 读者路径

| 我是谁 | 读什么 | 顺序 |
| --- | --- | --- |
| 第一次接触 | [README](../README.md) → [架构概览](architecture/overview.md) | 1 |
| 写业务代码 | [行为契约](architecture/contracts.md) → [构建](guides/build.md) → API 文档 | 2 |
| 改 Core / 加后端 | [模块边界](architecture/modules.md) → [接入新后端](guides/adding-backend.md) | 3 |
| 遇到限制或异常 | [已知差距](architecture/known-gaps.md) → [线协议](architecture/wire-protocol.md) | 4 |
| 要发版 | [发布流程](guides/releasing.md) | 5 |
| 想知道为什么这么设计 | [ADR 索引](decisions/README.md) | 6 |

## 架构（现在是什么）

| 文档 | 内容 | 维护角色 |
| --- | --- | --- |
| [overview.md](architecture/overview.md) | 定位、分层、决策摘要、术语表 | 架构 owner |
| [modules.md](architecture/modules.md) | 模块边界、依赖方向、target 对应 | 构建 owner |
| [contracts.md](architecture/contracts.md) | 生命周期、并发、回调、错误码、送达语义 | 模块 owner |
| [wire-protocol.md](architecture/wire-protocol.md) | FFRP 帧、发现文本协议、兼容性承诺 | 协议 owner |
| [known-gaps.md](architecture/known-gaps.md) | 唯一限制清单 | 架构 owner |

## 指南（怎么做）

| 文档 | 内容 | 维护角色 |
| --- | --- | --- |
| [build.md](guides/build.md) | 依赖、target 拓扑、开关、验证命令 | 构建 owner |
| [testing.md](guides/testing.md) | 测试分组、验收命令、故障注入 | 测试 owner |
| [adding-backend.md](guides/adding-backend.md) | 接入新后端的最小实现面 | Core owner |
| [releasing.md](guides/releasing.md) | 版本、tag、Releases 模板 | 发布 owner |
| [documentation.md](guides/documentation.md) | 本规范 | 文档 owner |

## 决策（当时为什么）

见 [ADR 索引](decisions/README.md)。
```

`docs/decisions/README.md`：

```markdown
# 架构决策记录（ADR）索引

> 编号单调递增、永不复用、永不重排。新增 ADR 必须同时：分配下一个未使用编号、在本文档登记一行、被至少一篇 `architecture/*` 引用。

| 编号 | 标题 | 状态 | 日期 | 取代关系 |
| --- | --- | --- | --- | --- |
| 0000 | [模板（非 ADR）](0000-template.md) | — | — | 不参与状态校验 |
| 0001 | [用 ADR 记录架构决策](0001-record-architecture-decisions.md) | Accepted | 2026-11-24 | — |
| 0002 | [传输与发现拆为两个可替换契约](0002-split-transport-and-discovery-contracts.md) | Accepted | 2026-11-24 | — |
| 0003 | [传输按四角色建模](0003-four-role-transport-interfaces.md) | Accepted | 2026-11-24 | 取代 0002 中的 `ISocket` 部分 |
| 0004 | [发现层采用 EndpointRecord 记录模型](0004-endpoint-record-discovery-model.md) | Accepted | 2026-11-24 | — |
| 0005 | [每 Client 单 I/O 执行者、Reader/Service 串行回调](0005-single-io-executor-per-client.md) | Accepted | 2026-11-24 | — |
| 0006 | [不引入 ZeroMQ 第三后端](0006-no-third-backend-zmq.md) | Accepted（No-go） | 2026-11-24 | — |
| 0007 | [文档分层与过程记录出库](0007-document-taxonomy-and-process-exit.md) | Proposed | 2026-11-24 | — |
```

> `0000-template.md` 必须按上表登记为「模板（非 ADR）」，CI 的索引双向比对据此排除（见 6.3 注 2）。

### A.4 归档与 tag

`git rm` 只删除工作区文件，历史仍可用 `git show <tag>:docs/implementation.md` 取回。删前务必确认团队知识库副本可访问，并在 tag 注释中记录归档地址：

```bash
git tag -a refactor-complete-archive -m "过程文档归档：<知识库链接>；提交清单见 git log"
```

> 读者路径表只维护一份：`docs/README.md` 模板（A.3）即权威版本，本文不再重复列第二份（原附录 B 已并入 A.3，避免两处维护同一事实）。

