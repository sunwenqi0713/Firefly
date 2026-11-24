## 变更说明

<!-- 一句话说明改了什么、为什么。不要贴 commit hash 或 Issue 号，那些写在提交信息或 PR 描述里。 -->

## 验证

<!-- 跑了哪些测试/命令，结果如何。构建或行为变化请贴关键输出。 -->

## 文档检查（逐项勾选；确实不适用时写"不适用：原因"）

- [ ] 公开接口（`include/firefly/**`）的签名或语义有变化时，Doxygen 注释已同步，`doxygen Doxyfile` 无告警
- [ ] 行为、生命周期、错误码或送达语义有变化时，`docs/architecture/contracts.md` 已在**同一个 PR** 更新
- [ ] 线协议或发现文本协议有变化时，先有 ADR，且 `docs/architecture/wire-protocol.md` 已更新
- [ ] 操作步骤有变化（构建命令、CMake 选项、验证入口）时，`docs/guides/*` 已同步
- [ ] 目录、target 或依赖有变化时，`docs/architecture/modules.md` 与 `docs/guides/build.md` 已同步
- [ ] 新增或修复了限制时，`docs/architecture/known-gaps.md` 已更新
- [ ] 属于不可逆或跨模块决策时，已新增 ADR，编号已登记到 `docs/decisions/README.md`
- [ ] 新增文档已登记到 `docs/README.md`；同一事实没有第三处重复描述
- [ ] 未把过程信息写进仓库：无提交 hash、批次号、Issue/PR 号、审查人姓名、临时版本标记
- [ ] `README.md` 仍不超过 120 行；没有手工修改 `docs/api/`（该目录是生成产物，不入库）

> 本地先行自检：`python scripts/check_docs.py --strict`，与 CI 的 `docs` 工作流一致。
