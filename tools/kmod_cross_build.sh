#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# 在 x86_64 宿主上交叉编译 ARM64 内核模块（weaknet_cpu_stat / weaknet_softirq），
# 产物可直接在目标板上 insmod。
#
# 为什么需要这个脚本：板端「linux-headers」包里的 scripts/ 目录自带
# **aarch64 二进制**（fixdep、modpost），宿主机跑不了（Exec format error）。
# 而 headers 包里没有顶层 Kconfig，也无法 make prepare 重建它们。
# 于是本脚本在头文件缓存里就地重建这两个 kbuild 宿主工具为 x86-64 原生版本。
#
# 用法：
#   tools/kmod_cross_build.sh                    # 默认输出到 dist-arm64/kmod/
#   tools/kmod_cross_build.sh --out /tmp/x       # 指定输出目录
#   tools/kmod_cross_build.sh --refresh-headers  # 重新从板端同步头文件
#
# 环境变量：
#   BOARD       板端 SSH 目标（默认 board，见 CLAUDE.md 的隧道约定）
#   KMOD_CACHE  头文件缓存目录（默认 $HOME/.cache/weaknet-kmod-headers）
#
# 只产出 .ko，不改动仓库工作区。

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BOARD="${BOARD:-board}"
CACHE="${KMOD_CACHE:-$HOME/.cache/weaknet-kmod-headers}"
OUT=""
REFRESH=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --out) OUT="$2"; shift 2 ;;
        --refresh-headers) REFRESH=1; shift ;;
        -h|--help) sed -n '2,25p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "未知参数: $1" >&2; exit 2 ;;
    esac
done

log()  { echo "[kmod] $*"; }
fail() { echo "[kmod] 失败: $*" >&2; exit 1; }

# ---- 1. 取得目标板的内核版本与头文件 ----------------------------------------
# 头文件只能从目标板取（必须与目标内核逐字一致，否则 vermagic 对不上）。
# 已缓存过就直接复用，**不要求板子在线** —— 这样 CI/无板环境也能出包。
if [[ "$REFRESH" -eq 0 && -d "$CACHE" ]]; then
    # 缓存里可能有多份内核版本，取最新的一份
    CACHED="$(ls -1d "$CACHE"/*/ 2>/dev/null | grep -v -- '-build/$' | sort -V | tail -1 || true)"
    if [[ -n "$CACHED" && -f "${CACHED}Module.symvers" ]]; then
        REL="$(basename "$CACHED")"
        KDIR="${CACHED%/}"
        log "复用已缓存的头文件：$KDIR（内核 $REL）"
    fi
fi

if [[ -z "${REL:-}" ]]; then
    REL="$(ssh "$BOARD" 'uname -r' 2>/dev/null | tr -d '\r\n')" || true
    [[ -n "$REL" ]] || fail "既无本地头文件缓存，也无法通过 ssh $BOARD 取内核版本。
  先在有板的机器上跑一次本脚本（把 --refresh-headers 打开），
  或显式设置 KMOD_CACHE 指向已缓存的头文件目录。"
    KDIR="$CACHE/$REL"

    if [[ "$REFRESH" -eq 1 || ! -d "$KDIR/scripts" ]]; then
        log "从 $BOARD 同步 /usr/src/linux-headers-$REL（约 59 MB）"
        mkdir -p "$KDIR"
        rsync -az --delete -e ssh "$BOARD:/usr/src/linux-headers-$REL/" "$KDIR/" \
            || fail "同步内核头文件失败"
    fi
fi

[[ -f "$KDIR/Module.symvers" ]] || fail "缓存里没有 Module.symvers，无法做符号解析"
[[ -f "$KDIR/include/generated/autoconf.h" ]] || fail "缓存里缺少 include/generated"

# ---- 2. 重建 kbuild 宿主工具（x86-64 原生）----------------------------------
#
# 只有两个工具会真的被执行：
#   scripts/basic/fixdep    —— 生成 .cmd 依赖文件
#   scripts/mod/modpost     —— 解析符号、写 .mod.c、生成 vermagic
# 其余（dtc / kallsyms / extract-cert …）外部模块构建用不到。
#
# 关键坑：不能简单地 -I 把内核树头文件插到前面，否则 include/linux/types.h
# 会覆盖 glibc 的 stdint.h，产生 "conflicting types for uint64_t"；
# 也不能完全不指定，否则新版发行版 /usr/include/linux/mei_uuid.h 里的
# uuid_le typedef 会与 file2alias.c 自带的同名定义冲突。
# 正解是 -idirafter：内核树排在系统头之后，只在系统头缺符号时补位。

need_rebuild() {
    local f="$1"
    [[ -f "$f" ]] || return 0
    file -b "$f" 2>/dev/null | grep -q 'x86-64' && return 1
    return 0
}

if need_rebuild "$KDIR/scripts/basic/fixdep"; then
    log "重建 fixdep（x86-64 原生）"
    gcc -O2 -Wall -Wmissing-prototypes -Wstrict-prototypes \
        -o "$KDIR/scripts/basic/fixdep" "$KDIR/scripts/basic/fixdep.c" \
        || fail "fixdep 编译失败"
fi

if need_rebuild "$KDIR/scripts/mod/modpost"; then
    log "重建 modpost（x86-64 原生）"
    gcc -O2 -fomit-frame-pointer -std=gnu89 \
        -Wall -Wmissing-prototypes -Wstrict-prototypes \
        -idirafter "$KDIR/include" \
        -idirafter "$KDIR/arch/arm64/include" \
        -I "$KDIR/scripts/mod" \
        -o "$KDIR/scripts/mod/modpost" \
        "$KDIR/scripts/mod/modpost.c" \
        "$KDIR/scripts/mod/file2alias.c" \
        "$KDIR/scripts/mod/sumversion.c" \
        || fail "modpost 编译失败"
fi

for t in "$KDIR/scripts/basic/fixdep" "$KDIR/scripts/mod/modpost"; do
    file -b "$t" | grep -q 'x86-64' || fail "$t 仍然不是 x86-64 二进制"
done

# ---- 3. 交叉编译 ------------------------------------------------------------
OUT="${OUT:-$ROOT/dist-arm64/kmod}"
BUILD="$CACHE/$REL-build"
rm -rf "$BUILD"
mkdir -p "$BUILD" "$OUT"

# 只把源文件和 Makefile 拷进临时构建目录，保持仓库工作区干净
cp "$ROOT"/kmod/*.c "$ROOT"/kmod/*.h "$ROOT"/kmod/Makefile "$BUILD"/

log "交叉编译（ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-）"
make -C "$BUILD" -j"$(nproc)" \
    ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KDIR="$KDIR" \
    > "$BUILD/build.log" 2>&1 \
    || { tail -40 "$BUILD/build.log" >&2; fail "编译失败（完整日志 $BUILD/build.log）"; }

# ---- 4. 校验并安装产物 ------------------------------------------------------
#
# vermagic 是唯一能决定"这份 .ko 能不能被目标内核接受"的字段：
# 内核在 load_module() 里逐字比对，不一致直接 -ENOEXEC。
# 构建成功≠可加载，所以这里必须显式校验。

EXPECT_VERMAGIC="vermagic=$REL SMP preempt mod_unload aarch64"

shopt -s nullglob
kos=("$BUILD"/*.ko)
shopt -u nullglob
[[ ${#kos[@]} -gt 0 ]] || fail "没有产出任何 .ko"

for k in "${kos[@]}"; do
    vm="$(aarch64-linux-gnu-strings "$k" | grep -m1 '^vermagic=' || true)"
    if [[ "$vm" != "$EXPECT_VERMAGIC" ]]; then
        fail "$(basename "$k") vermagic 不匹配
  期望: $EXPECT_VERMAGIC
  实际: ${vm:-<缺失>}"
    fi
    if ! file -b "$k" | grep -q 'ARM aarch64'; then
        fail "$(basename "$k") 不是 ARM aarch64 目标文件"
    fi
    install -m 0644 "$k" "$OUT/"
    log "  $(basename "$k")  vermagic 校验通过"
done

log "完成：$(ls -1 "$OUT"/*.ko | wc -l) 个模块 → $OUT"
