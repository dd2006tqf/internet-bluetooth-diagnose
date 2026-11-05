#!/usr/bin/env bash
# ============================================================================
# qt_dashboard/tools/fetch-qt-arm64-sysroot.sh
#
# 从 Debian bullseye 源下载 arm64 的 Qt5 开发包，解压进 ARM64 交叉编译 sysroot。
#
# ## 为什么需要这个脚本
#
# 宿主机（Ubuntu 24.04）可以 `apt install qtbase5-dev` 直接验证 x86 构建，
# 但 ARM64 产物必须用 /opt/sysroot-bullseye-arm64，而那个 sysroot 是**解压树**
# 不是 apt 系统，里面没有 Qt 头文件。
#
# 板端直装 qtbase5-dev 已排除（libqt5gui5 与 libqt5gui5-gles 互斥，会拆 200+ KDE 包），
# 但这里下载的是**解压到 sysroot**，不安装到任何运行中的系统，无此副作用。
#
# ## 安全边界
#
# 1. 只解压到 $SYSROOT，不碰开发板、不碰宿主机 /usr
# 2. **排除 libc6 / libgcc-s1 / 核心系统包** —— sysroot 里的 glibc 2.31 是
#    交叉编译正确性的根基，被 bullseye 包覆盖会破坏已有的 OpenSSL/curl 构建
# 3. 只解压运行库与 dev 包，不装 apt/dpkg 元数据
#
# ## 用法
#
#   ./qt_dashboard/tools/fetch-qt-arm64-sysroot.sh            # 默认 sysroot
#   SYSROOT=/path/to/sysroot ./fetch-qt-arm64-sysroot.sh      # 覆盖
#   ./qt_dashboard/tools/fetch-qt-arm64-sysroot.sh --dry-run  # 只打印不下载
# ============================================================================

set -euo pipefail

SYSROOT="${SYSROOT:-/opt/sysroot-bullseye-arm64}"
MIRROR="${MIRROR:-https://mirrors.aliyun.com/debian}"
SUITE="${SUITE:-bullseye}"
# DEB_ARCH 是 Debian 的包架构名（Packages.gz 路径用），
# TRIPLE 是文件系统三元组（usr/lib/<triple> 用）。两者不同：arm64 vs aarch64-linux-gnu。
# 注意不要用 ARCH 这个名字：它是 make 的历史变量，会被环境残留污染。
DEB_ARCH="${DEB_ARCH:-arm64}"
TRIPLE="${TRIPLE:-aarch64-linux-gnu}"
WORKDIR="${WORKDIR:-/tmp/qt-arm64-sysroot}"

DRY_RUN=0
for arg in "$@"; do
    case "$arg" in
        --dry-run) DRY_RUN=1 ;;
        -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
        *) echo "未知参数: $arg" >&2; exit 2 ;;
    esac
done

log() { printf '[qt-arm64] %s\n' "$*"; }
die() { printf '[qt-arm64] 错误: %s\n' "$*" >&2; exit 1; }

[ -d "$SYSROOT/usr/lib/$TRIPLE" ] || die "sysroot 不像 bullseye 树: $SYSROOT (缺 usr/lib/$TRIPLE)"

# ---------------------------------------------------------------------------
# 必须保留的包：这些是 sysroot 的地基，绝不能被 Debian 包覆盖
#
# 覆盖 libc6 会导致：交叉编译链原本链接的是经过验证的 2.31，
# 被替换后行为不可预期；且已有的 OpenSSL / curl 私有构建也可能失效。
# ---------------------------------------------------------------------------
PROTECTED_PKGS=(
    libc6 libc-bin libc-dev-bin libgcc-s1 gcc-10-base libstdc++6
    libcrypt1 zlib1g libssl1.1 libssl-dev
)

# ---------------------------------------------------------------------------
# 需要的 Qt 包（只含编译看板必需的）
#
# qtbase5-dev      头文件 + .prl + mkspecs
# qtbase5-dev-tools  moc / uic / rcc
# libqt5*5        运行库
# libqt5*5-dev    各模块的 .so 符号链接
# ---------------------------------------------------------------------------
ROOT_PKGS=(
    qtbase5-dev
    qtbase5-dev-tools
    libqt5core5a
    libqt5gui5
    libqt5widgets5
    libqt5dbus5
    libqt5network5
    libqt5concurrent5
    libqt5printsupport5
    libqt5sql5
    libqt5test5
    libqt5xml5
)

mkdir -p "$WORKDIR"
cd "$WORKDIR"

# ---------------------------------------------------------------------------
# 1. 拉取包索引
# ---------------------------------------------------------------------------
PKGS_GZ="$WORKDIR/Packages.gz"
if [ ! -s "$PKGS_GZ" ]; then
    log "下载包索引 ($DEB_ARCH/$SUITE)..."
    curl -fsSL --retry 3 -o "$PKGS_GZ" \
        "$MIRROR/dists/$SUITE/main/binary-$DEB_ARCH/Packages.gz" \
        || die "包索引下载失败"
fi
log "索引大小: $(du -h "$PKGS_GZ" | cut -f1)"

# ---------------------------------------------------------------------------
# 2. 递归解析依赖，算出完整包集（排除受保护包）
# ---------------------------------------------------------------------------
log "解析依赖树..."
mapfile -t DEB_LIST < <(python3 - "$PKGS_GZ" "${ROOT_PKGS[@]}" <<'PYEOF'
import gzip, re, sys

pkgs_gz = sys.argv[1]
roots = sys.argv[2:]

protected = {
    "libc6", "libc-bin", "libc-dev-bin", "libgcc-s1", "gcc-10-base",
    "libstdc++6", "libcrypt1", "zlib1g", "libssl1.1", "libssl-dev",
    # 这些是 sysroot 已有且不该被动的基础件
    "base-files", "dpkg", "debconf", "ucf", "perl", "perl-base",
    "perl-modules-5.32", "libperl5.32", "coreutils", "sensible-utils",
    "x11-common", "lsb-base", "fontconfig-config", "shared-mime-info",
}

pools = {}
cur = {}
with gzip.open(pkgs_gz, "rt", encoding="utf-8", errors="replace") as f:
    for line in f:
        line = line.rstrip("\n")
        if not line:
            if "Package" in cur and "Filename" in cur:
                pools[cur["Package"]] = (cur["Filename"], cur.get("Depends", ""))
            cur = {}
            continue
        if line.startswith("Package:"):
            cur["Package"] = line.split(":", 1)[1].strip()
        elif line.startswith("Filename:"):
            cur["Filename"] = line.split(":", 1)[1].strip()
        elif line.startswith("Depends:"):
            cur["Depends"] = line.split(":", 1)[1].strip()

seen = set()
order = []

def resolve(name):
    if name in seen or name in protected:
        return
    seen.add(name)
    if name not in pools:
        return
    fname, deps = pools[name]
    order.append((name, fname))
    if not deps:
        return
    for part in deps.split(","):
        alt = part.split("|")[0].strip()
        dep = re.sub(r"\s*\(.*?\)", "", alt).strip().split(":")[0]
        if dep:
            resolve(dep)

for r in roots:
    resolve(r)

for name, fname in order:
    print(f"{name}\t{fname}")
PYEOF
)

[ "${#DEB_LIST[@]}" -gt 0 ] || die "依赖解析结果为空"

log "需下载 ${#DEB_LIST[@]} 个包（已排除受保护包）"
if [ "$DRY_RUN" -eq 1 ]; then
    for entry in "${DEB_LIST[@]}"; do
        printf '  %s\n' "${entry%%$'\t'*}"
    done
    log "dry-run 结束，未下载"
    exit 0
fi

# ---------------------------------------------------------------------------
# 3. 下载并解压
# ---------------------------------------------------------------------------
FAILED=0
for entry in "${DEB_LIST[@]}"; do
    pkg="${entry%%$'\t'*}"
    rel="${entry##*$'\t'}"
    deb="$WORKDIR/$(basename "$rel")"

    if [ ! -f "$deb" ]; then
        # 单包 curl 比 apt 并发下载快得多（实测 1.6 s/MB）
        if ! curl -fsSL --retry 3 --max-time 300 -o "$deb" "$MIRROR/$rel"; then
            printf '  下载失败: %s\n' "$pkg" >&2
            FAILED=$((FAILED + 1))
            continue
        fi
    fi

    # dpkg-deb -x 只解压文件，不执行任何 maintainer script
    if ! dpkg-deb -x "$deb" "$SYSROOT" 2>/dev/null; then
        printf '  解压失败: %s\n' "$pkg" >&2
        FAILED=$((FAILED + 1))
        continue
    fi
    printf '  ok  %s\n' "$pkg"
done

if [ "$FAILED" -gt 0 ]; then
    die "$FAILED 个包失败"
fi

# ---------------------------------------------------------------------------
# 4. 验证
# ---------------------------------------------------------------------------
log "验证安装结果..."

QTINC="$SYSROOT/usr/include/$TRIPLE/qt5"
[ -d "$QTINC" ] || die "Qt 头文件目录不存在: $QTINC"

MOC="$SYSROOT/usr/lib/qt5/bin/moc"
if [ -x "$MOC" ]; then
    log "moc: $MOC"
else
    # 二进制是 arm64，宿主机无法执行；只要文件存在即可由 qemu 或交叉调用
    [ -f "$MOC" ] && log "moc 文件存在（arm64，宿主机不可直接执行）" \
        || log "警告: 未找到 moc"
fi

QTLIB="$SYSROOT/usr/lib/$TRIPLE"
for lib in libQt5Core.so.5 libQt5Gui.so.5 libQt5Widgets.so.5; do
    [ -e "$QTLIB/$lib" ] && printf '  ok  %s\n' "$lib" \
        || printf '  缺失 %s\n' "$lib" >&2
done

log "完成。sysroot: $SYSROOT"
log "构建命令："
cat <<EOF

  cmake -S qt_dashboard -B build-qt-arm64 \\
    -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-arm64-cross.cmake \\
    -DCMAKE_BUILD_TYPE=Debug \\
    -DWEAKN_DASHBOARD_WITH_UI=ON \\
    -DWEAKNET_CLIENT_LIB=<build-arm64>/client/lib/libweaknet.so
  cmake --build build-qt-arm64 -j1

EOF
