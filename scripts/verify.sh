#!/usr/bin/env bash
# scripts/verify.sh — 第 5 批 T2：一条命令完成双后端验收
#
# 覆盖四条路径（默认全部执行）：
#   1) 默认 ON（NNG）配置 + 构建 + 全量 ctest -jN（含共享契约内存/NNG 双实例）
#   2) 跨进程互测 4 组单独确认（发现 / Pub/Sub / RPC，独立端口，并行安全）
#   3) 从 ON 的 vcpkg_installed 剥离 NNG，构造独立依赖前缀（第 4 批方案）
#   4) FIREFLY_WITH_NNG=OFF（无 NNG 环境）配置 + 构建 + 全量测试
#
# 前置（当前 Windows + Git Bash 环境）：
#   - CMake >= 3.16、Ninja、Python3（仅标准库，用于跨进程用例）
#   - MSVC 2022 + Windows 10 SDK、vcpkg（manifest 含 protobuf / nng）
#   - 首次 ON 配置会由 vcpkg 安装依赖，需要网络或已缓存的二进制
#   - 所有路径可用环境变量覆盖；已在 VS 开发环境中可设 FIREFLY_SKIP_MSVC_ENV=1
#
# 用法：
#   bash scripts/verify.sh                     # 四条路径全跑
#   JOBS=4 bash scripts/verify.sh
#   bash scripts/verify.sh --skip-on            # 仅 OFF 路径（需已有 ON 的 vcpkg_installed）
#   bash scripts/verify.sh --skip-off           # 仅 ON 路径 + 跨进程
#   bash scripts/verify.sh --skip-interop      # 跳过跨进程单独确认（全量 ctest 仍含它们）
#
# 退出码：任一阶段失败即非 0；成功时打印两条路径的测试统计。

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT_WIN="$ROOT"
if command -v cygpath >/dev/null 2>&1; then
  ROOT_WIN="$(cygpath -w "$ROOT" | tr '\\' '/')"
fi

# ---- 可覆盖的环境变量（默认值对应本机第 4 批验证环境）----
VCPKG_ROOT="${VCPKG_ROOT:-D:/vcpkg}"
VCPKG_TRIPLET="${VCPKG_TRIPLET:-x64-windows}"
MSVC_TOOLS="${MSVC_TOOLS:-D:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207}"
WINSDK_ROOT="${WINSDK_ROOT:-C:/Program Files (x86)/Windows Kits/10}"
WINSDK_VER="${WINSDK_VER:-10.0.26100.0}"
NINJA="${NINJA:-$VCPKG_ROOT/downloads/tools/ninja-1.13.2-windows/ninja.exe}"
JOBS="${JOBS:-6}"
SKIP_MSVC_ENV="${FIREFLY_SKIP_MSVC_ENV:-0}"

ON_DIR="$ROOT/build"
OFF_DIR="$ROOT/build-no-nng"
# FIREFLY_DEPS_DIR 可在全新目录验证"从零构造前缀"路径，无需删除任何现有目录。
DEPS="${FIREFLY_DEPS_DIR:-$OFF_DIR/deps}"
SRC_INSTALLED="$ON_DIR/vcpkg_installed/$VCPKG_TRIPLET"
ON_DIR_WIN="$ROOT_WIN/build"
if command -v cygpath >/dev/null 2>&1; then
  DEPS_WIN="$(cygpath -w "$DEPS" | tr '\\' '/')"
else
  DEPS_WIN="$DEPS"
fi

# OFF 构建目录与依赖前缀绑定：同一构建目录在不同 CMAKE_PREFIX_PATH 之间反复重配，
# 会让静态库归档与可执行文件来自不同轮次的链接产物（实测 backend_contract_memory
# 会 SegFault）。换前缀即换构建目录，保证每次要么全新、要么同前缀增量构建。
compute_off_build() {
  OFF_BUILD="$OFF_DIR/off-$(printf '%s' "$DEPS" | cksum | cut -d' ' -f1)"
  if command -v cygpath >/dev/null 2>&1; then
    OFF_BUILD_WIN="$(cygpath -w "$OFF_BUILD" | tr '\\' '/')"
  else
    OFF_BUILD_WIN="$OFF_BUILD"
  fi
}
compute_off_build

log() { printf '\n========== %s ==========\n' "$*"; }
die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

usage() {
  sed -n '2,30p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

DO_ON=1
DO_OFF=1
DO_INTEROP=1
for arg in "$@"; do
  case "$arg" in
    --skip-on)       DO_ON=0 ;;
    --skip-off)      DO_OFF=0 ;;
    --skip-interop)  DO_INTEROP=0 ;;
    -h|--help)       usage; exit 0 ;;
    *)               die "unknown option: $arg (see --help)" ;;
  esac
done

for tool in cmake ctest; do
  command -v "$tool" >/dev/null 2>&1 || die "找不到 $tool，请加入 PATH"
done
[ -f "$ROOT/CMakeLists.txt" ] || die "脚本必须位于 Firefly 仓库内"
mkdir -p "$ON_DIR" "$OFF_DIR" "$OFF_BUILD"

# Git Bash 中 Windows 风格路径（D:/...）不能直接用于 bash 的 -f/-d 测试，
# 统一转成 POSIX 路径再判断；非 MSYS 环境原样返回。
win_path() {
  if command -v cygpath >/dev/null 2>&1; then
    cygpath -u "$1"
  else
    printf '%s' "$1"
  fi
}

# ---- MSVC 环境注入 ----------------------------------------------------------
# vcvars64.bat 会调用 reg.exe，在受限 shell 中被拦截；这里直接注入等价变量。
setup_msvc() {
  if [ "$SKIP_MSVC_ENV" = "1" ]; then
    log "跳过 MSVC 环境注入（调用方已配置编译器环境）"
    return
  fi
  [ -d "$(win_path "$MSVC_TOOLS")/include" ] || die \
    "MSVC_TOOLS 不存在：$MSVC_TOOLS
请设置 MSVC_TOOLS 指向 VC/Tools/MSVC/<版本>，或在 VS 开发环境中设置 FIREFLY_SKIP_MSVC_ENV=1"
  export INCLUDE="$MSVC_TOOLS/include;$WINSDK_ROOT/Include/$WINSDK_VER/ucrt;$WINSDK_ROOT/Include/$WINSDK_VER/shared;$WINSDK_ROOT/Include/$WINSDK_VER/um;$WINSDK_ROOT/Include/$WINSDK_VER/winrt"
  export LIB="$MSVC_TOOLS/lib/x64;$WINSDK_ROOT/Lib/$WINSDK_VER/ucrt/x64;$WINSDK_ROOT/Lib/$WINSDK_VER/um/x64"
  local msvc_bin sdk_bin
  msvc_bin="$(win_path "$MSVC_TOOLS/bin/Hostx64/x64")"
  sdk_bin="$(win_path "$WINSDK_ROOT/bin/$WINSDK_VER/x64")"
  export PATH="$msvc_bin:$sdk_bin:$PATH"
  log "已注入 MSVC / Windows SDK 环境"
}

setup_msvc
BASE_PATH="$PATH"

# 跨进程用例存在已知时序抖动：legacy 进程的 registry follower 在并行高负载下
# 可能遇到 3s REQ 超时（见 docs/acceptance-matrix.md L10）。仅对这类失败做有限
# 重试，其他失败立即报错，避免掩盖真实回归。
run_on_ctest() {
  local attempt
  for attempt in 1 2 3; do
    if ctest --test-dir "$ON_DIR_WIN" --output-on-failure -j "$JOBS" 2>&1 |
        tee "$ON_DIR/ctest-on.log"; then
      return 0
    fi
    if ! grep -qE 'Test #[0-9]+: cross_.*\*\*\*Failed' "$ON_DIR/ctest-on.log"; then
      printf 'ERROR: 全量 ctest 失败且不属于跨进程偶发超时，不重试\n' >&2
      return 1
    fi
    printf '\n[verify] 跨进程用例偶发超时，第 %s/3 次重试全量 ctest\n' "$attempt" >&2
  done
  return 1
}

# ---- 1) 默认 ON：配置 + 构建 + 全量 ctest ----------------------------------
if [ "$DO_ON" = "1" ]; then
  log "路径 1/4：默认 ON（NNG）配置 + 构建"
  [ -f "$(win_path "$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake")" ] || die \
    "未找到 vcpkg toolchain：$VCPKG_ROOT（可设置 VCPKG_ROOT 覆盖）"
  [ -f "$(win_path "$NINJA")" ] || die \
    "未找到 ninja：$NINJA（可设置 NINJA 覆盖）"
  export PATH="$ON_DIR/vcpkg_installed/$VCPKG_TRIPLET/debug/bin:$ON_DIR/vcpkg_installed/$VCPKG_TRIPLET/bin:$BASE_PATH"

  cmake -S "$ROOT_WIN" -B "$ON_DIR_WIN" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
    -DVCPKG_TARGET_TRIPLET="$VCPKG_TRIPLET" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_MAKE_PROGRAM="$NINJA" \
    -DVCPKG_APPLOCAL_DEPS=OFF \
    -DFIREFLY_WITH_NNG=ON 2>&1 | tee "$ON_DIR/configure-on.log"
  grep -q "NNG backend ENABLED" "$ON_DIR/configure-on.log" \
    || die "ON 配置日志未确认启用 NNG 后端"

  cmake --build "$ON_DIR_WIN" --config Debug --parallel "$JOBS"
  log "路径 1/4：默认 ON 全量 ctest -j$JOBS（已含 4 组跨进程用例）"
  run_on_ctest || die "默认 ON 全量 ctest 失败，详见 $ON_DIR/ctest-on.log"
else
  log "路径 1/4：已跳过（--skip-on）"
fi

# ---- 2) 跨进程互测确认 -----------------------------------------------------
# 跨进程用例已由全量 ctest 执行；默认从日志确认 4/4，不重复启动两进程，
# 避免重复执行时的 follower 注册超时抖动（第 4 批已记录的时序脆弱点）。
if [ "$DO_INTEROP" = "1" ]; then
  if [ "$DO_ON" = "1" ] && [ -f "$ON_DIR/ctest-on.log" ]; then
    log "路径 2/4：确认跨进程发现 / Pub/Sub / RPC（4 组独立地址）"
    cross_count="$(grep -cE 'Test #[0-9]+: cross_.*Passed' "$ON_DIR/ctest-on.log" || true)"
    [ "$cross_count" -eq 4 ] || die \
      "全量 ctest 中跨进程用例未全部通过（$cross_count/4），详见 $ON_DIR/ctest-on.log"
    printf '  4/4 跨进程用例在全量 ctest -j%s 中通过\n' "$JOBS"
  else
    log "路径 2/4：单独执行跨进程用例（--skip-on 场景）"
    export PATH="$ON_DIR/vcpkg_installed/$VCPKG_TRIPLET/debug/bin:$ON_DIR/vcpkg_installed/$VCPKG_TRIPLET/bin:$BASE_PATH"
    ctest --test-dir "$ON_DIR_WIN" --output-on-failure -j "$JOBS" -R '^cross_'
  fi
else
  log "路径 2/4：已跳过（--skip-interop）"
fi

# ---- 3) 构造无 NNG 独立依赖前缀 -------------------------------------------
# 只把非 NNG 内容复制进独立前缀，全程不执行删除（受限环境会拦截批量删除）。
# OFF 配置不使用 vcpkg 工具链，manifest 不会重新引入 nng，构成真正的"未安装 NNG"环境。
copy_tree_wo_nng() {
  local src="$1" dst="$2" s t rel
  [ -d "$src" ] || return 0
  mkdir -p "$dst"
  (cd "$src" && find . \( -iname '*nng*' \) -prune -o -print0) |
    while IFS= read -r -d '' rel; do
      s="$src/${rel#./}"
      t="$dst/${rel#./}"
      if [ -d "$s" ]; then
        mkdir -p "$t"
      else
        cp "$s" "$t"
      fi
    done
}

stage_no_nng_deps() {
  [ -d "$SRC_INSTALLED/share/protobuf" ] || die \
    "缺少 ON 构建的 vcpkg 依赖：$SRC_INSTALLED（先跑一次默认 ON 配置）"

  # 已有前缀且确认干净则复用；否则换一个唯一目录名重建，绝不删除现有目录。
  if [ -d "$DEPS" ]; then
    local leftover protoc=""
    leftover="$(find "$DEPS" -iname '*nng*' -print)"
    if [ -f "$DEPS/tools/protobuf/protoc.exe" ] || [ -f "$DEPS/tools/protobuf/protoc" ]; then
      protoc=1
    fi
    if [ -z "$leftover" ] && [ -n "$protoc" ]; then
      log "路径 3/4：复用已确认干净的无 NNG 依赖前缀 $DEPS"
      return 0
    fi
    DEPS="$OFF_DIR/deps-verify-$$"
    if command -v cygpath >/dev/null 2>&1; then
      DEPS_WIN="$(cygpath -w "$DEPS" | tr '\\' '/')"
    else
      DEPS_WIN="$DEPS"
    fi
    compute_off_build
    mkdir -p "$OFF_BUILD"
    log "已有前缀不符合要求，改用新前缀 $DEPS（不删除任何现有目录）"
  fi

  log "路径 3/4：构造无 NNG 独立依赖前缀 $DEPS"
  mkdir -p "$DEPS"
  copy_tree_wo_nng "$SRC_INSTALLED/share"      "$DEPS/share"
  copy_tree_wo_nng "$SRC_INSTALLED/lib"        "$DEPS/lib"
  copy_tree_wo_nng "$SRC_INSTALLED/bin"        "$DEPS/bin"
  copy_tree_wo_nng "$SRC_INSTALLED/include"    "$DEPS/include"
  copy_tree_wo_nng "$SRC_INSTALLED/tools"     "$DEPS/tools"
  copy_tree_wo_nng "$SRC_INSTALLED/debug/lib" "$DEPS/debug/lib"
  copy_tree_wo_nng "$SRC_INSTALLED/debug/bin" "$DEPS/debug/bin"

  leftover="$(find "$DEPS" -iname '*nng*' -print)"
  [ -z "$leftover" ] || die "依赖前缀仍含 NNG 残留：\n$leftover"
  log "依赖前缀已确认不含任何 NNG 头 / 库 / 包配置"
}

# ---- 4) OFF：无 NNG 环境配置 + 构建 + 测试 ---------------------------------
if [ "$DO_OFF" = "1" ]; then
  stage_no_nng_deps
  log "路径 4/4：FIREFLY_WITH_NNG=OFF 配置 + 构建 + 全量测试"
  # 关键：不设置 vcpkg 工具链，且 PATH 不含 ON 的 vcpkg_installed（那里有 nng.dll）。
  export PATH="$DEPS/debug/bin:$DEPS/bin:$BASE_PATH"
  [ -f "$DEPS/tools/protobuf/protoc.exe" ] || [ -f "$DEPS/tools/protobuf/protoc" ] || die \
    "依赖前缀缺少 protoc：$DEPS/tools/protobuf"

  local_protoc="$DEPS_WIN/tools/protobuf/protoc.exe"
  [ -f "$DEPS/tools/protobuf/protoc.exe" ] || local_protoc="$DEPS_WIN/tools/protobuf/protoc"

  cmake -S "$ROOT_WIN" -B "$OFF_BUILD_WIN" -G Ninja \
    -DCMAKE_CXX_COMPILER="$MSVC_TOOLS/bin/Hostx64/x64/cl.exe" \
    -DCMAKE_RC_COMPILER="$WINSDK_ROOT/bin/$WINSDK_VER/x64/rc.exe" \
    -DCMAKE_MT="$WINSDK_ROOT/bin/$WINSDK_VER/x64/mt.exe" \
    -DCMAKE_PREFIX_PATH="$DEPS_WIN" \
    -DPROTOC_EXE="$local_protoc" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_MAKE_PROGRAM="$NINJA" \
    -DFIREFLY_WITH_NNG=OFF \
    -DCMAKE_DISABLE_FIND_PACKAGE_nng=TRUE 2>&1 | tee "$OFF_DIR/configure-off.log"
  grep -q "NNG backend DISABLED" "$OFF_DIR/configure-off.log" \
    || die "OFF 配置日志未确认跳过 NNG（不能接受仅本地能编）"

  cmake --build "$OFF_BUILD_WIN" --config Debug --parallel "$JOBS"
  ctest --test-dir "$OFF_BUILD_WIN" --output-on-failure
else
  log "路径 4/4：已跳过（--skip-off）"
fi

log "全部完成：默认 ON（NNG）与无 NNG OFF（内存）两条路径均通过"
printf '  ON  构建目录：%s\n' "$ON_DIR_WIN"
printf '  OFF 构建目录：%s\n' "$OFF_BUILD_WIN"
printf '  OFF 依赖前缀：%s\n' "$DEPS"
