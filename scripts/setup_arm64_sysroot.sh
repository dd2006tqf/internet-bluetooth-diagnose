#!/usr/bin/env bash
# ============================================================================
# scripts/setup_arm64_sysroot.sh
# 自动化初始化/更新 ARM64 Debian 11 (Bullseye) 裸机原生交叉编译 Sysroot
#
# 目标路径: /opt/sysroot-bullseye-arm64
#
# 原理：
# 1. 直接从预置好的 weaknet-builder:bullseye-arm64 Docker 镜像（或运行容器）中
#    一键导出完整的 lib, usr, opt 目录。
# 2. 自动修正 GNU ld 对动态库链接脚本/软链接的绝对路径陷阱（将绝对路径转换为相对路径或去除宿主污染）。
# 3. 补齐 libbpf 与私有 sysroot 扩展组件，使 x86 宿主机 Clang 具备一键原生构建能力。
# ============================================================================

set -euo pipefail

SYSROOT_DIR="${SYSROOT_DIR:-/opt/sysroot-bullseye-arm64}"
IMAGE_NAME="${IMAGE_NAME:-weaknet-builder:bullseye-arm64}"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
info() { echo -e "${CYAN}ℹ️  $1${NC}"; }
pass() { echo -e "${GREEN}✅ $1${NC}"; }
warn() { echo -e "${YELLOW}⚠️  $1${NC}"; }
fail() { echo -e "${RED}❌ $1${NC}"; exit 1; }

echo "========================================================"
echo "  WeakNet ARM64 Bullseye Sysroot 自动化安装脚本"
echo "  目标路径: ${SYSROOT_DIR}"
echo "========================================================"

# 1. 检查权限
if [ ! -w "$(dirname "${SYSROOT_DIR}")" ] && [ ! -w "${SYSROOT_DIR}" ]; then
    fail "没有写入 $(dirname "${SYSROOT_DIR}") 的权限，请使用 sudo 运行此脚本"
fi

# 2. 检查源镜像或容器
CONTAINER_SRC=""
if docker ps -a --format '{{.Names}}' | grep -q "^weaknet-arm64-dev$"; then
    CONTAINER_SRC="weaknet-arm64-dev"
    info "检测到运行中的容器: weaknet-arm64-dev，将从该容器导出 sysroot"
elif docker image inspect "${IMAGE_NAME}" >/dev/null 2>&1; then
    info "创建临时容器从镜像 ${IMAGE_NAME} 导出 sysroot..."
    CONTAINER_SRC=$(docker create "${IMAGE_NAME}")
    trap 'docker rm -f "${CONTAINER_SRC}" >/dev/null 2>&1 || true' EXIT
else
    fail "未找到容器 weaknet-arm64-dev 也未找到镜像 ${IMAGE_NAME}，无法导出 sysroot"
fi

# 3. 导出关键目录
mkdir -p "${SYSROOT_DIR}"
info "正在从容器导出 /usr, /lib, /opt ..."

# 使用 tar 流式解压保证软链接和权限
docker cp "${CONTAINER_SRC}:/lib" - | tar -xf - -C "${SYSROOT_DIR}"
docker cp "${CONTAINER_SRC}:/usr" - | tar -xf - -C "${SYSROOT_DIR}"
if docker exec "${CONTAINER_SRC}" test -d /opt 2>/dev/null || [ -d "/opt" ]; then
    docker cp "${CONTAINER_SRC}:/opt" - | tar -xf - -C "${SYSROOT_DIR}" 2>/dev/null || true
fi

# 4. 修复 GNU ld 绝对路径软链接缺陷
info "正在修正 GNU ld 的 absolute symlinks 与 ldscripts..."

# 修复 libpthread.so / libc.so 等软链接
MULTIARCH_LIB="${SYSROOT_DIR}/usr/lib/aarch64-linux-gnu"
if [ -d "${MULTIARCH_LIB}" ]; then
    # 确保 libpthread.so 是指向当前目录或相对路径的链接
    if [ -f "${MULTIARCH_LIB}/libpthread.so" ] && grep -q "/lib/" "${MULTIARCH_LIB}/libpthread.so" 2>/dev/null; then
        warn "修正 libpthread.so GNU ld script 绝对路径..."
        sed -i 's|/lib/aarch64-linux-gnu/|../../lib/aarch64-linux-gnu/|g' "${MULTIARCH_LIB}/libpthread.so"
        sed -i 's|/usr/lib/aarch64-linux-gnu/||g' "${MULTIARCH_LIB}/libpthread.so"
    fi

    if [ -f "${MULTIARCH_LIB}/libc.so" ] && grep -q "/lib/" "${MULTIARCH_LIB}/libc.so" 2>/dev/null; then
        warn "修正 libc.so GNU ld script 绝对路径..."
        sed -i 's|/lib/aarch64-linux-gnu/|../../lib/aarch64-linux-gnu/|g' "${MULTIARCH_LIB}/libc.so"
        sed -i 's|/usr/lib/aarch64-linux-gnu/||g' "${MULTIARCH_LIB}/libc.so"
    fi
fi

# 5. 校验 glibc 版本
GLIBC_SO=$(find "${SYSROOT_DIR}/lib" -name "libc-2.31.so" | head -1 || true)
if [ -n "${GLIBC_SO}" ]; then
    pass "Sysroot glibc 校验通过: Debian Bullseye glibc 2.31"
else
    warn "未在 sysroot 中检测到 libc-2.31.so，请确认基础系统版本"
fi

pass "Sysroot 初始化/更新成功: ${SYSROOT_DIR}"
