#!/usr/bin/env python3
"""阶段三零泄漏验收：公共头不得（直接或传递）包含传输层/注册表实现细节。

规则（对 firefly/ 下所有头文件生效）：
  - 位于 firefly/detail/ 的头文件为内部头，自身允许包含 nng / discovery；
  - 其余头文件（公共头）的 #include（含传递闭包）不得触及：
      * <nng/...> 或 "nng/..."
      * detail/*_impl.h
      * detail/discovery.h（注册中心实现）
  - 注释中提及 nng 是契约说明，不视为泄漏，只检查 #include 行。

运行：python tools/check_no_leak.py [仓库根目录]
退出码：0=通过，1=发现泄漏。
"""
import pathlib
import re
import sys

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]')
FORBIDDEN_TARGET = re.compile(r'(^|/)detail/.*_impl\.h$|(^|/)detail/discovery\.h$')
NNG_TARGET = re.compile(r'^nng/')


def main() -> int:
    root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else '.').resolve()
    source = root / 'firefly'
    include_map = {}
    for path in sorted(source.rglob('*')):
        if path.suffix not in ('.h', '.hpp'):
            continue
        rel = path.relative_to(root).as_posix()
        targets = []
        for line in path.read_text(encoding='utf-8-sig').splitlines():
            match = INCLUDE_RE.match(line)
            if match:
                targets.append(match.group(1))
        include_map[rel] = (path, targets)

    violations = []
    for rel, (path, targets) in include_map.items():
        if rel.startswith('firefly/detail/'):
            continue
        visited = set()
        stack = [(rel, targets, rel)]
        while stack:
            owner, includes, origin = stack.pop()
            for target in includes:
                if NNG_TARGET.match(target) or FORBIDDEN_TARGET.search(target):
                    violations.append(
                        f'{origin}: via {owner}: include "{target}"')
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
    print('零泄漏检查通过：公共头无 nng / *_impl.h / discovery 传递包含')
    return 0


if __name__ == '__main__':
    sys.exit(main())
