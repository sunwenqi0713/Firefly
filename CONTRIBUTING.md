# 贡献指南

- **文档规则**：先读 [docs/guides/documentation.md](docs/guides/documentation.md)。公共接口注释、ADR 准入条件、同一 PR 内更新文档的要求都在那里。
- **文档自检**：`python scripts/check_docs.py --strict`，与 CI 的 `docs` 工作流跑同一条命令；改了公共头另需 `doxygen Doxyfile` 无告警。
- **验证**：提交前跑 `bash scripts/verify.sh`（或至少 `ctest --preset default`）；测试细节见 [docs/guides/testing.md](docs/guides/testing.md)。
- **PR**：模板中的文档检查项需逐条勾选或说明不适用；不可逆或跨模块决策请附 ADR。

问题反馈请走 [GitHub Issues](https://github.com/sunwenqi0713/Firefly/issues)，注明提交号、操作系统、编译器、后端配置与最小复现步骤。
