#!/usr/bin/env bash
# ============================================================================
# release.sh — WeakNet 官方固件包自动化构建、签名与发布工具（OTA Phase 2）
#
# 功能：
#   1. 版本校验（校验 VERSION 或接受指定版本号）
#   2. 调用 ARM64 裸机原生交叉编译生成 dist-arm64
#   3. 使用 tools/weaknet-local-update.sh package 进行打包与 manifest 生成
#   4. 使用 ECDSA (P-256) 发布私钥（secrets/release_priv.pem）生成分离式签名 (.sig)
#   5. 生成制品汇总校验清单（artifacts/SHA256SUMS）
#
# 用法：
#   ./tools/release.sh                  # 使用当前 VERSION 构建签名固件
#   ./tools/release.sh 1.1.0            # 升级 VERSION 至 1.1.0 并构建签名固件
#   ./tools/release.sh --skip-build     # 使用现有 dist-arm64 重新打包与签名
# ============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SECRETS_DIR="${ROOT}/secrets"
CERTS_DIR="${ROOT}/certs"
ARTIFACTS_DIR="${ROOT}/artifacts"
PRIV_KEY="${SECRETS_DIR}/release_priv.pem"
PUB_KEY="${CERTS_DIR}/release_pub.pem"
SYSROOT_DIR="${SYSROOT_DIR:-/opt/sysroot-bullseye-arm64}"

SKIP_BUILD=false
NEW_VERSION=""

for arg in "$@"; do
    case "$arg" in
        --skip-build) SKIP_BUILD=true ;;
        --help|-h)
            echo "用法: $0 [version] [--skip-build]"
            echo "示例: $0 1.1.0"
            exit 0
            ;;
        *)
            if [[ "$arg" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
                NEW_VERSION="$arg"
            else
                echo "未知参数或版本号格式错误（需 X.Y.Z）: $arg"
                exit 1
            fi
            ;;
    esac
done

# ---- 检查私钥 ----
if [ ! -f "${PRIV_KEY}" ]; then
    echo "❌ 缺少发布私钥：${PRIV_KEY}"
    echo "请先运行 ./tools/gen-release-keys.sh 生成发布密钥对，或配置 CI Secret"
    exit 1
fi

# ---- 更新或获取版本号 ----
if [ -n "${NEW_VERSION}" ]; then
    echo "${NEW_VERSION}" > "${ROOT}/VERSION"
    echo "ℹ️  已更新仓库根 VERSION 为: ${NEW_VERSION}"
fi

VERSION="$(tr -d '[:space:]' < "${ROOT}/VERSION" 2>/dev/null || echo "")"
if ! [[ "${VERSION}" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "❌ 仓库根 VERSION 缺失或非法（需一行 X.Y.Z）：'${VERSION}'"
    exit 1
fi

echo "=============================================="
echo "  WeakNet 官方固件发布构建 — v${VERSION}"
echo "=============================================="

# ---- Step 1: ARM64 裸机原生交叉编译 ----
if [ "$SKIP_BUILD" = false ]; then
    echo ""
    echo "===== Step 1: ARM64 原生交叉编译 (Clang + Bullseye Sysroot) ====="
    if [ ! -d "${SYSROOT_DIR}" ] || [ ! -f "${ROOT}/cmake/toolchain-arm64-cross.cmake" ]; then
        echo "❌ 缺少 Sysroot (${SYSROOT_DIR}) 或工具链文件"
        exit 1
    fi

    (
        set -euo pipefail
        cd "${ROOT}"
        cmake -B build-arm64-native \
              -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-arm64-cross.cmake \
              -DCMAKE_BUILD_TYPE=Debug \
              -DBUILD_EBPF=ON >/dev/null

        cmake --build build-arm64-native --target weaknet-dbus-server history_query_tool weaknet weaknet_cli test_client_bin test_ebpf ebpf -j"$(nproc)" >/dev/null

        rm -rf dist-arm64
        mkdir -p dist-arm64/server/bin dist-arm64/server/build dist-arm64/server/test \
                 dist-arm64/client/bin dist-arm64/client/lib dist-arm64/lib

        install -m 0755 build-arm64-native/server/weaknet-dbus-server dist-arm64/server/bin/
        install -m 0755 build-arm64-native/server/history_query_tool dist-arm64/server/bin/
        install -m 0755 build-arm64-native/server/test/test_ebpf dist-arm64/server/test/
        for f in build-arm64-native/server/build/*.bpf.o; do install -m 0644 "$f" dist-arm64/server/build/ 2>/dev/null || true; done
        install -m 0755 build-arm64-native/client/bin/test_client_bin dist-arm64/client/bin/test-client
        install -m 0755 build-arm64-native/client/bin/weaknet_cli dist-arm64/client/bin/weaknet-cli
        install -m 0755 client/weaknet-offline-diag dist-arm64/client/bin/weaknet-offline-diag
        install -m 0644 build-arm64-native/client/lib/libweaknet.so dist-arm64/client/lib/
        install -m 0755 "AI-assisted analysis/edge_network_rag.py" dist-arm64/edge_network_rag.py
        install -m 0644 "AI-assisted analysis/network_knowledge_base.py" dist-arm64/network_knowledge_base.py
        cp -a "${SYSROOT_DIR}/usr/local/lib/libbpf.so"* dist-arm64/lib/ 2>/dev/null || true

        install -m 0644 "${ROOT}/VERSION" dist-arm64/VERSION
        install -m 0755 "${ROOT}/tools/weaknet-local-update.sh" dist-arm64/weaknet-local-update.sh
    )
    echo "✅ 编译完成，产物已打包至 dist-arm64"
else
    echo "ℹ️  跳过编译步骤，直接使用现有 dist-arm64"
fi

if [ ! -d "${ROOT}/dist-arm64" ]; then
    echo "❌ dist-arm64 目录不存在"
    exit 1
fi

# ---- Step 2: 打包、清单生成与签名 ----
echo ""
echo "===== Step 2: 打包与 ECDSA (P-256) 签名 ====="
mkdir -p "${ARTIFACTS_DIR}"
PKG_NAME="weaknet-${VERSION}-arm64.tar.zst"
PKG_PATH="${ARTIFACTS_DIR}/${PKG_NAME}"
SIG_PATH="${PKG_PATH}.sig"

# 临时复制 dist 目录以准备发布包
TEMP_DIST="$(mktemp -d /tmp/wn-rel-dist-XXXXXX)"
cp -rL "${ROOT}/dist-arm64/." "${TEMP_DIST}/"

WEAKNET_RELEASE_PRIV="${PRIV_KEY}" "${ROOT}/tools/weaknet-local-update.sh" package "${TEMP_DIST}" "${VERSION}" "${PKG_PATH}"
rm -rf "${TEMP_DIST}"

if [ ! -f "${PKG_PATH}" ] || [ ! -f "${SIG_PATH}" ]; then
    echo "❌ 打包或签名失败"
    exit 1
fi

echo "✅ 固件包与签名已就位："
echo "  固件: ${PKG_PATH} ($(du -h "${PKG_PATH}" | cut -f1))"
echo "  签名: ${SIG_PATH}"

# ---- Step 3: 本地公钥自验 ----
echo ""
echo "===== Step 3: 本地公钥自检验签 ====="
if [ -f "${PUB_KEY}" ]; then
    openssl dgst -sha256 -verify "${PUB_KEY}" -signature "${SIG_PATH}" "${PKG_PATH}"
    echo "✅ 自验证通过：公钥与签名完全匹配！"
fi

# ---- Step 4: 生成 SHA256SUMS ----
echo ""
echo "===== Step 4: 生成发布制品 SHA256SUMS ====="
(
    cd "${ARTIFACTS_DIR}"
    sha256sum "${PKG_NAME}" "${PKG_NAME}.sig" > SHA256SUMS
)
cat "${ARTIFACTS_DIR}/SHA256SUMS"

echo ""
echo "=============================================="
echo "🎉 发布固件构建完成！所有产物保存在 ${ARTIFACTS_DIR}/"
echo "=============================================="
