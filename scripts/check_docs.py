#!/usr/bin/env python3
"""Firefly 文档门禁检查。

规则来源：docs/guides/documentation.md 第六步（6.2 PR 模板 / 6.3 CI 检查）。

检查项：
  1. 站内相对链接可达（含 GitHub 风格锚点）
  2. 孤儿文档：docs/**/*.md 必须被某篇 Markdown 以相对路径引用
  3. ADR 索引与文件双向一致，状态取值合法
  4. 失效标签扫描（阶段号/批次号/TODO(phase)/@threadsafe 等）
  5. README.md 行数上限
  6. docs/api 未纳入版本控制、不存在 docs/history
  7. 建议项：文件名禁用词（--strict 时升级为失败）

用法：
    python scripts/check_docs.py           # 只让「必需」项导致失败
    python scripts/check_docs.py --strict  # 「建议」项也导致失败

退出码：0 通过；1 存在失败项；2 运行环境问题（如不是 git 仓库）。
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import unicodedata

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# 跳过扫描的目录（构建产物、本地工具、依赖）
SKIP_DIRS = {"build", "build-mem", "build-no-nng", "vcpkg_installed", ".git",
             ".workbuddy", "docs/api", "tests/gen"}

README_MAX_LINES = 120

# 孤儿检查免检的入口文件
ORPHAN_EXEMPT = {
    "docs/README.md",
    "docs/decisions/README.md",
    "docs/decisions/0000-template.md",
}

# 整文件豁免失效标签扫描：本文件必须引用反例词才能说明规则
STALE_TAG_EXEMPT = {"docs/guides/documentation.md"}

STALE_TAG_PATTERNS = [
    (r"@threadsafe", "@threadsafe 不是 Doxygen 标签，改用 @par 线程安全"),
    (r"阶段[一二三四五]", "阶段号属于过程标记"),
    (r"第 ?[0-9] ?批", "批次号属于过程标记"),
    (r"TODO\(phase", "TODO(phaseN) 属于过程标记，公共头只用 @todo"),
    (r"TODO\(@", "TODO(@某人) 指向个人，会过期"),
    (r"FIXME\(#", "FIXME(#123) 是指向 Issue 的死引用"),
    (r"@version", "@version 标记不可维护"),
]

ADR_STATUS_OK = {"Proposed", "Accepted", "Deprecated", "Rejected", "—"}
ADR_SUPERSEDED_RE = re.compile(r"^Superseded by ADR-(\d{4})$")

FENCE_RE = re.compile(r"^\s*(```|~~~)")
LINK_RE = re.compile(r"!?\[[^\]]*\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)")
HEADING_RE = re.compile(r"^(#{1,6})\s+(.*)$")
ADR_ROW_RE = re.compile(r"^\|\s*(\d{4})\s*\|(.*)$")
ADR_FILE_RE = re.compile(r"^(\d{4})-.+\.md$")
BANNED_NAME_RE = re.compile(r"(notes|misc|tmp|draft|v2|final|old)", re.IGNORECASE)

failures: list[str] = []
warnings: list[str] = []


def fail(msg: str) -> None:
    failures.append(msg)


def warn(msg: str) -> None:
    warnings.append(msg)


def rel(path: str) -> str:
    return os.path.relpath(path, ROOT).replace(os.sep, "/")


def iter_markdown() -> list[str]:
    out = []
    for base, dirs, files in os.walk(ROOT):
        rbase = rel(base) if base != ROOT else ""
        dirs[:] = [d for d in dirs
                   if (os.path.join(rbase, d) if rbase else d) not in SKIP_DIRS]
        for name in files:
            if name.endswith(".md"):
                out.append(os.path.normpath(os.path.join(base, name)))
    return sorted(out)


def iter_public_headers() -> list[str]:
    out = []
    inc = os.path.join(ROOT, "include")
    for base, _dirs, files in os.walk(inc):
        for name in files:
            if name.endswith(".h"):
                out.append(os.path.join(base, name))
    return sorted(out)


def read_text(path: str) -> str:
    with open(path, encoding="utf-8-sig") as fh:
        return fh.read()


def strip_fences(text: str) -> str:
    """把围栏代码块内容替换为空行，保留行号。"""
    out, inside = [], False
    for line in text.splitlines():
        if FENCE_RE.match(line):
            inside = not inside
            out.append("")
            continue
        out.append("" if inside else line)
    return "\n".join(out)


def strip_check_regions(text: str) -> str:
    """清空 <!-- docs-check:off --> 与 <!-- docs-check:on --> 之间的内容。"""
    out, skipping = [], False
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("<!--") and "docs-check:off" in stripped:
            skipping = True
            out.append("")
            continue
        if stripped.startswith("<!--") and "docs-check:on" in stripped:
            skipping = False
            out.append("")
            continue
        out.append("" if skipping else line)
    return "\n".join(out)


def slugify(heading: str) -> str:
    text = re.sub(r"`([^`]*)`", r"\1", heading)
    text = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", text)
    text = unicodedata.normalize("NFKC", text).strip().lower()
    kept = []
    for ch in text:
        if ch.isalnum() or ch in "-_":
            kept.append(ch)
        elif ch.isspace():
            kept.append("-")
    return "".join(kept)


def heading_anchors(path: str) -> set[str]:
    anchors: set[str] = set()
    for line in strip_fences(read_text(path)).splitlines():
        m = HEADING_RE.match(line)
        if not m:
            continue
        slug = slugify(m.group(2))
        anchors.add(slug)
        # GitHub 对重复标题追加 -1、-2……
        for i in range(1, 6):
            anchors.add(f"{slug}-{i}")
    return anchors


def check_links(md_files: list[str]) -> None:
    for path in md_files:
        src_dir = os.path.dirname(path)
        text = strip_fences(read_text(path))
        for m in LINK_RE.finditer(text):
            target = m.group(1)
            if target.startswith(("http://", "https://", "mailto:", "tel:")):
                continue
            frag = ""
            if "#" in target:
                target, frag = target.split("#", 1)
            if not target:
                continue  # 纯锚点，同文件内跳转
            resolved = os.path.normpath(os.path.join(src_dir, target))
            if rel(resolved).startswith("docs/api"):
                continue  # 生成产物，本地不要求存在
            if not os.path.exists(resolved):
                fail(f"断链：{rel(path)} -> {m.group(1)}")
                continue
            if frag and resolved.endswith(".md"):
                if frag not in heading_anchors(resolved):
                    fail(f"锚点不存在：{rel(path)} -> {m.group(1)}")


def collect_references(md_files: list[str]) -> set[str]:
    refs: set[str] = set()
    for path in md_files:
        src_dir = os.path.dirname(path)
        text = strip_fences(read_text(path))
        for m in LINK_RE.finditer(text):
            target = m.group(1)
            if target.startswith(("http://", "https://", "mailto:", "tel:")):
                continue
            target = target.split("#", 1)[0]
            if not target:
                continue
            refs.add(rel(os.path.normpath(os.path.join(src_dir, target))))
    return refs


def check_orphans(md_files: list[str]) -> None:
    refs = collect_references(md_files)
    for path in md_files:
        name = rel(path)
        if name.startswith("docs/") and name not in ORPHAN_EXEMPT:
            if name not in refs:
                fail(f"孤儿文档：{name} 未被任何文档引用（应在 docs/README.md 登记）")
    if os.path.isdir(os.path.join(ROOT, "docs", "history")):
        fail("存在 docs/history/：过程记录一律归档到团队 Wiki，不在仓库内保留只读归档")


def check_adr_index() -> None:
    dec_dir = os.path.join(ROOT, "docs", "decisions")
    index = os.path.join(dec_dir, "README.md")
    if not os.path.isdir(dec_dir):
        fail("缺少 docs/decisions/ 目录")
        return
    if not os.path.exists(index):
        fail("缺少 docs/decisions/README.md 索引")
        return

    files = {}
    for name in os.listdir(dec_dir):
        m = ADR_FILE_RE.match(name)
        if m and m.group(1) != "0000":
            files[m.group(1)] = name

    rows = {}
    for line in read_text(index).splitlines():
        m = ADR_ROW_RE.match(line.strip())
        if not m:
            continue
        num = m.group(1)
        if num == "0000":
            continue
        cells = [c.strip() for c in m.group(2).split("|")]
        rows[num] = cells

    for num in sorted(set(files) - set(rows)):
        fail(f"ADR 未登记到索引：docs/decisions/{files[num]}")
    for num in sorted(set(rows) - set(files)):
        fail(f"索引登记了不存在的 ADR：{num}")

    for num, cells in sorted(rows.items()):
        if len(cells) < 2:
            fail(f"ADR {num} 的索引行格式不完整，应为 | 编号 | 标题 | 状态 | 日期 | 取代关系 |")
            continue
        linked = LINK_RE.search(cells[0])
        if not linked:
            fail(f"ADR {num} 的标题列缺少指向文件的链接")
        else:
            expect = os.path.basename(linked.group(1).split("#", 1)[0])
            if files.get(num) and expect != files[num]:
                fail(f"ADR {num} 索引链接指向 {expect}，实际文件为 {files[num]}")

        status = cells[1].strip("` ")
        # 允许在主状态后加括号补充，例如 Accepted（No-go）；只校验主状态
        base = re.split(r"[（(]", status, maxsplit=1)[0].strip("` ")
        if base not in ADR_STATUS_OK and not ADR_SUPERSEDED_RE.match(base):
            fail(f"ADR {num} 状态取值非法：{status!r}"
                 f"（允许 {sorted(ADR_STATUS_OK)} 或 'Superseded by ADR-XXXX'）")
        m = ADR_SUPERSEDED_RE.match(base)
        if m and m.group(1) not in files:
            fail(f"ADR {num} 声明被 ADR-{m.group(1)} 取代，但该文件不存在")


def check_stale_tags() -> None:
    targets = list(iter_public_headers())
    targets += [p for p in iter_markdown() if rel(p).startswith("docs/")]
    for path in targets:
        name = rel(path)
        if name in STALE_TAG_EXEMPT:
            continue
        text = strip_check_regions(read_text(path))
        if not name.startswith("docs/"):
            text = strip_fences(text)
        for lineno, line in enumerate(text.splitlines(), start=1):
            for pattern, reason in STALE_TAG_PATTERNS:
                if re.search(pattern, line):
                    fail(f"失效标签：{name}:{lineno} 命中 {pattern!r}（{reason}）")


def check_readme() -> None:
    readme = os.path.join(ROOT, "README.md")
    if not os.path.exists(readme):
        fail("缺少根 README.md")
        return
    lines = read_text(readme).splitlines()
    if len(lines) > README_MAX_LINES:
        fail(f"README.md 有 {len(lines)} 行，超过上限 {README_MAX_LINES}；"
             f"细节应移入 docs/")


def check_git_tracking() -> None:
    try:
        out = subprocess.run(["git", "ls-files", "docs/api"], cwd=ROOT,
                             capture_output=True, text=True, check=True)
    except (OSError, subprocess.CalledProcessError):
        warn("无法执行 git ls-files，跳过 docs/api 入库检查")
        return
    if out.stdout.strip():
        fail("docs/api 下有文件被纳入版本控制；生成产物不应入库")


def check_banned_names(md_files: list[str]) -> None:
    for path in md_files:
        stem = os.path.splitext(os.path.basename(path))[0]
        m = BANNED_NAME_RE.search(stem)
        if m:
            warn(f"文件名含禁用词 {m.group(0)!r}：{rel(path)}（建议改名，避免临时文档堆积）")


def check_external_links(md_files: list[str]) -> None:
    """外链可达性检查（建议项，仅供定时任务告警）。

    只做一次 HEAD 请求，失败后回退 GET；超时与 DNS 失败记为告警而非失败，
    因为外链抖动不应阻断开发。
    """
    import urllib.error
    import urllib.request

    seen: set[str] = set()
    for path in md_files:
        text = strip_fences(read_text(path))
        for m in LINK_RE.finditer(text):
            url = m.group(1)
            if not url.startswith(("http://", "https://")) or url in seen:
                continue
            seen.add(url)
            req = urllib.request.Request(url, method="HEAD",
                                         headers={"User-Agent": "firefly-docs-check"})
            try:
                urllib.request.urlopen(req, timeout=10)
            except urllib.error.HTTPError as exc:
                if exc.code >= 400:
                    warn(f"外链返回 {exc.code}：{url}（{rel(path)}）")
            except Exception as exc:  # 网络超时、DNS 失败等
                warn(f"外链不可达：{url}（{rel(path)}）: {exc}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Firefly 文档门禁检查")
    parser.add_argument("--strict", action="store_true",
                        help="把「建议」项也视为失败")
    parser.add_argument("--external", action="store_true",
                        help="额外检查外部 http(s) 链接（仅告警，不阻断）")
    args = parser.parse_args()

    md_files = iter_markdown()
    check_links(md_files)
    check_orphans(md_files)
    check_adr_index()
    check_stale_tags()
    check_readme()
    check_git_tracking()
    check_banned_names(md_files)
    if args.external:
        check_external_links(md_files)

    for msg in warnings:
        print(f"[warn] {msg}")
    for msg in failures:
        print(f"[fail] {msg}")

    if failures:
        print(f"\n文档门禁未通过：{len(failures)} 项失败，{len(warnings)} 项告警")
        return 1
    if args.strict and warnings:
        print(f"\n严格模式下告警升级为失败：{len(warnings)} 项")
        return 1
    print(f"文档门禁通过：检查 {len(md_files)} 篇 Markdown、"
          f"{len(iter_public_headers())} 个公共头，{len(warnings)} 项告警")
    return 0


if __name__ == "__main__":
    sys.exit(main())
