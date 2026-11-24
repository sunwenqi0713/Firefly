# 发布流程

版本、tag 与发布说明的规范。兼容性承诺的内容见 [../architecture/wire-protocol.md](../architecture/wire-protocol.md) §4，本文只管**动作**。

## 1. 版本策略

| 项 | 规则 |
| --- | --- |
| 版本号 | `MAJOR.MINOR.PATCH`。当前未发布稳定版前，`0.x` 内允许公共 API 的源码级变更，但必须走 ADR 并在发布说明写明 |
| 承诺范围 | **源码兼容**：公开头路径、`firefly::firefly`、端点创建方式、公开结果语义 |
| 不承诺 | 二进制 ABI（见 [known-gaps.md](../architecture/known-gaps.md) G11）；跨异构字节序部署 |
| 废弃流程 | 先标 `[[deprecated("use X instead")]]` 并至少保留一个 MINOR 周期，再删除；注释必须写替代方案 |
| 线协议 | 只允许追加式变更；破坏性变更必须写 ADR 并给出迁移窗口 |

## 2. 发布前检查

- [ ] `bash scripts/verify.sh` 全绿（ON 全量 + 跨进程 4 组 + 无 NNG OFF 全量）。
- [ ] `doxygen Doxyfile` 无 warning；`docs/api/` 未被误提交。
- [ ] `docs/architecture/contracts.md` 与 `known-gaps.md` 已反映本次变更。
- [ ] 公共 API 或线协议有变更时，对应 ADR 已 Accepted 并登记在 [decisions/README.md](../decisions/README.md)。
- [ ] `README.md` 仍 ≤120 行，能力表与实际一致。
- [ ] `docs/README.md` 索引无遗漏、无死链。
- [ ] 跨进程互测与旧版本二进制互通结论已写入发布说明（能互通 / 不通、原因、迁移方式）。

## 3. tag 规范

只用 **annotated tag**，名称 `v<MAJOR.MINOR.PATCH>`，不打 lightweight tag。tag 消息模板：

```text
Firefly v0.5.0

范围：<一句话>
兼容性：公共 API 源码兼容；FFRP 帧 / 发现文本协议 / pub-sub 载荷未变
        旧版 v0.4 二进制双向互通：是 / 否（原因）
验收：scripts/verify.sh 全绿（ON 30/30、OFF 24/24、cross 4/4）
过程归档：<团队 Wiki 链接>
```

命令：

```bash
git tag -a v0.5.0 -F release-msg.txt
git push origin v0.5.0
```

tag 消息里的「过程归档」指向团队 Wiki 的重构过程文档页；仓库内**不保留**过程文档（理由见 [documentation.md](documentation.md) 第一步）。

## 4. 发布说明（GitHub Releases）

正文固定五节，长度控制在 40 行内：

```markdown
## 亮点
<3 条以内，面向使用者，不写内部重构细节>

## 新增
- <能力 / API / 选项>

## 变更与废弃
- <行为变化；废弃项及替代方案>

## 兼容性
- 源码兼容：是 / 否（说明）
- 线协议：未变 / 追加字段 <名称>；旧版互通：是 / 否

## 升级步骤
<需要用户动手的最短步骤；无则写「直接替换即可」
```

不要把逐条提交清单贴进 Releases（`git log` 已是权威记录）；需要过程背景时链接 Wiki。

## 5. 分支与推送

- 单主干 `main`。发布即打 tag，不开长期 release 分支。
- 推送前确认 `git status` 干净、CI 绿；**禁止** `push --force`（确需改写历史时单独评审并留记录）。
- 需要展示用的压缩历史时，用独立分支承载，不替换 `main`；在发布说明中注明「压缩分支非权威历史」。
