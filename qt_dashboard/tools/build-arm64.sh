#!/usr/bin/env bash
# ============================================================================
# qt_dashboard/tools/build-arm64.sh
#
# 为 ARM64 开发板构建 Qt 看板（宿主机原生交叉编译，不需要 Docker/QEMU 跑构建）。
#
# ## 关键技巧：moc 用 x86 的，编译器用 arm64 的
#
# Qt 的 moc 是**构建期工具**，它的产物 moc_*.cpp 只是把 Q_OBJECT 宏展开成
# 普通 C++ 源码，**与目标架构无关**。因此：
#
#   - moc 用宿主机 x86 版（qtbase5-dev-tools，apt 装，零卸载）
#   - 编译用 clang --target=aarch64-linux-gnu + sysroot
#
# 这样就不需要 qemu-user-static、不需要 binfmt 注册、不需要在容器里跑构建。
#
# 已验证：x86 moc 5.15.13 的产物交给 arm64 clang 编译，10/10 单元零警告通过；
# 链接产物 glibc 需求仅 GLIBC_2.17，远低于板端 2.31。
#
# ## 前置条件
#
#   1. sysroot 里有 arm64 的 Qt5 开发文件
#      → ./qt_dashboard/tools/fetch-qt-arm64-sysroot.sh
#   2. 宿主机有 x86 moc
#      → sudo apt install qtbase5-dev-tools   （8 个包，零卸载）
#   3. 已构建 arm64 的 libweaknet.so
#      → cmake -B build-arm64-native -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-arm64-cross.cmake
#
# ## 用法
#
#   ./qt_dashboard/tools/build-arm64.sh
#   BUILD_DIR=/tmp/my-build ./qt_dashboard/tools/build-arm64.sh
# ============================================================================

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

SYSROOT="${SYSROOT:-/opt/sysroot-bullseye-arm64}"
TRIPLE="${TRIPLE:-aarch64-linux-gnu}"
OUT="${BUILD_DIR:-$REPO_ROOT/build-qt-arm64}"
WEAKNET_LIB_DIR="${WEAKNET_LIB_DIR:-$REPO_ROOT/build-arm64-native/client/lib}"

QTINC="$SYSROOT/usr/include/$TRIPLE/qt5"
QTLIB="$SYSROOT/usr/lib/$TRIPLE"

log() { printf '[build-arm64] %s\n' "$*"; }
die() { printf '[build-arm64] 错误: %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# 前置检查：一次性说清缺什么，不要编译到一半才报错
# ---------------------------------------------------------------------------
log "前置检查..."

[ -d "$QTINC/QtWidgets" ] || die "sysroot 缺 Qt 头文件: $QTINC/QtWidgets
  先运行: ./qt_dashboard/tools/fetch-qt-arm64-sysroot.sh"

[ -e "$QTLIB/libQt5Widgets.so.5" ] || die "sysroot 缺 Qt 运行库: $QTLIB"

command -v moc >/dev/null 2>&1 || die "宿主机没有 moc
  先运行: sudo apt install qtbase5-dev-tools"

MOC_VERSION="$(moc -v 2>&1 | head -1)"
log "宿主机 moc: $MOC_VERSION"

[ -f "$WEAKNET_LIB_DIR/libweaknet.so" ] || die "缺 arm64 libweaknet.so: $WEAKNET_LIB_DIR
  先运行: cmake -B build-arm64-native -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-arm64-cross.cmake
          cmake --build build-arm64-native -j\$(nproc)"

command -v clang++ >/dev/null 2>&1 || die "缺 clang++"

# ---------------------------------------------------------------------------
# 编译
# ---------------------------------------------------------------------------

CXXFLAGS=(
    --target="$TRIPLE"
    --sysroot="$SYSROOT"
    --gcc-toolchain="$SYSROOT/usr"
    -stdlib=libstdc++
    -std=c++17
    -fPIC
    -Wall -Wextra
    -DQT_NO_DEBUG
    -I"$REPO_ROOT/qt_dashboard"
    -I"$REPO_ROOT/client"
    -I"$QTINC"
    -I"$QTINC/QtCore"
    -I"$QTINC/QtGui"
    -I"$QTINC/QtWidgets"
)

LDFLAGS=(
    --target="$TRIPLE"
    --sysroot="$SYSROOT"
    --gcc-toolchain="$SYSROOT/usr"
    -stdlib=libstdc++
)

mkdir -p "$OUT"
cd "$REPO_ROOT/qt_dashboard"

# ---------------------------------------------------------------------------
# 1. moc：x86 工具，产物与架构无关
# ---------------------------------------------------------------------------
log "生成 moc..."
for h in line_chart_widget data_fetch_thread main_window; do
    moc "ui/$h.hpp" -o "$OUT/moc_$h.cpp" \
        || die "moc 失败: $h"
    log "  moc $h"
done

# ---------------------------------------------------------------------------
# 2. 编译（-j1：QEMU/交叉环境下高并行度易出错，且单核即可快速完成）
# ---------------------------------------------------------------------------
log "编译（串行，避免并行问题）..."
SOURCES=(
    data/json_reader
    data/dashboard_data
    data/data_source
    ui/chart_geometry
    ui/theme
    ui/line_chart_widget
    ui/data_fetch_thread
    ui/main_window
    main
)
OBJECTS=()

for src in "${SOURCES[@]}"; do
    base="$(basename "$src")"
    obj="$OUT/$base.o"
    if ! clang++ "${CXXFLAGS[@]}" -c "$src.cpp" -o "$obj" 2>"$OUT/$base.err"; then
        printf '[build-arm64] 编译失败: %s\n' "$src.cpp" >&2
        head -20 "$OUT/$base.err" >&2
        exit 1
    fi
    OBJECTS+=("$obj")
    log "  $base"
done

for m in moc_line_chart_widget moc_data_fetch_thread moc_main_window; do
    obj="$OUT/$m.o"
    if ! clang++ "${CXXFLAGS[@]}" -c "$OUT/$m.cpp" -o "$obj" 2>"$OUT/$m.err"; then
        printf '[build-arm64] 编译失败: %s\n' "$m" >&2
        head -20 "$OUT/$m.err" >&2
        exit 1
    fi
    OBJECTS+=("$obj")
    log "  $m"
done

# ---------------------------------------------------------------------------
# 3. 链接
# ---------------------------------------------------------------------------
log "链接..."
clang++ "${LDFLAGS[@]}" -o "$OUT/weaknet_dashboard" "${OBJECTS[@]}" \
    -L"$QTLIB" -lQt5Widgets -lQt5Gui -lQt5Core -lQt5DBus \
    -L"$WEAKNET_LIB_DIR" -lweaknet \
    -Wl,-rpath-link,"$QTLIB" \
    -Wl,-rpath-link,"$SYSROOT/lib/$TRIPLE" \
    || die "链接失败"

# ---------------------------------------------------------------------------
# 4. 验证产物
# ---------------------------------------------------------------------------
log "验证..."
file "$OUT/weaknet_dashboard" | sed 's/^/  /'

MAX_GLIBC="$(readelf --version-info "$OUT/weaknet_dashboard" 2>/dev/null \
    | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)"
log "最高 glibc 需求: ${MAX_GLIBC:-未知}  （板端是 GLIBC_2.31）"

SIZE="$(stat -c%s "$OUT/weaknet_dashboard")"
log "产物大小: $SIZE 字节"

cat <<EOF

[build-arm64] 完成: $OUT/weaknet_dashboard

部署到开发板：
  scp $OUT/weaknet_dashboard board:/tmp/
  ssh board 'QT_QPA_PLATFORM=offscreen /tmp/weaknet_dashboard --help'

接显示器后（需要 /dev/dri 权限）：
  ssh board 'sudo QT_QPA_PLATFORM=eglfs /tmp/weaknet_dashboard'
EOF
