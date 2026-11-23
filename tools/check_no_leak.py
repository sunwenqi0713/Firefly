#!/usr/bin/env python3
"""零泄漏验收：公共头不得（直接或传递）包含 Core/后端实现细节。

目录调整后规则：
  - 公共头位于 include/firefly/（仍以 "firefly/..." 路径包含）；
  - 内部头位于 src/core/、src/backends/，以 "core/..." / "backends/..." 包含；
  - include/firefly/ 下所有头的传递闭包不得触及：
      * <nng/...> 或 "nng/..."
      * "core/..." / "backends/..."（传输角色、Registry、后端实现）
      * *_impl.h
  - 注释中提及 nng/Core 是契约说明，不视为泄漏，只检查 #include 行。

运行：python tools/check_no_leak.py [仓库根目录]
退出码：0=通过，1=发现泄漏。
"""
import pathlib
import re
import sys

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]')
FORBIDDEN = re.compile(r'^(core|backends)/|_impl\.h$|^nng/')


def main() -> int:
    root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else '.').resolve()
    public_root = root / 'include' / 'firefly'
    if not public_root.is_dir():
        print('零泄漏检查失败：缺少 include/firefly 公共头目录')
        return 1

    include_map = {}
    for path in sorted(public_root.rglob('*')):
        if path.suffix not in ('.h', '.hpp'):
            continue
        rel = 'firefly/' + path.relative_to(public_root).as_posix()
        targets = []
        for line in path.read_text(encoding='utf-8-sig').splitlines():
            match = INCLUDE_RE.match(line)
            if match:
                targets.append(match.group(1))
        include_map[rel] = (path, targets)

    violations = []
    for rel, (_, targets) in include_map.items():
        visited = set()
        stack = [(rel, targets, rel)]
        while stack:
            owner, includes, origin = stack.pop()
            for target in includes:
                if FORBIDDEN.search(target):
                    violations.append(f'{origin}: via {owner}: include "{target}"')
                    continue
                if not target.startswith('firefly/'):
                    continue
                child = include_map.get(target)
                if not child or target in visited:
                    continue
                visited.add(target)
                stack.append((target, child[1], origin))

    if violations:
        print('零泄漏检查失败：')
        print('\n'.join(sorted(set(violations))))
        return 1
    print('零泄漏检查通过：公共头无 core/backends/nng 传递包含')
    return 0


if __name__ == '__main__':
    sys.exit(main())
