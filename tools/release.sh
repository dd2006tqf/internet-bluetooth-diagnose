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

        cmake --build build-arm64-native --target weaknet-dbus-server history_query_tool weaknet weaknet_cli weaknet_health_guard test_client_bin test_ebpf test_crash_handler ebpf -j"$(nproc)" >/dev/null

        # ===== Qt 本地看板（可选产物）=====

        # 与 tools/ci.sh 同源：看板是独立 CMake 工程，不是所有部署都装显示器。

        # 失败**不中断**发布：看板不参与任何业务链路，服务端/客户端/eBPF 才是

        # 发布主体；编不出来就跳过，下面的 install 用 -f 判存在。

        #

        # 缺陷背景（C2）：此前 release.sh 完全不构建看板，于是「发布链出的

        # 固件」与「ci.sh 部署出的现场」内容不同 —— 同一版本号在两条路径下

        # 携带的文件集合不一样，OTA 包缺 weaknet-dashboard。

        DASHBOARD_BIN="build-qt-dashboard-arm64/weaknet_dashboard"
        if cmake -S qt_dashboard -B build-qt-dashboard-arm64 \
                 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-arm64-cross.cmake \
                 -DCMAKE_BUILD_TYPE=Release \
                 -DWEAKN_DASHBOARD_WITH_UI=ON \
                 -DWEAKNET_CLIENT_LIB="${ROOT}/build-arm64-native/client/lib/libweaknet.so" \
                 >/dev/null 2>&1 \
           && cmake --build build-qt-dashboard-arm64 -j"$(nproc)" >/dev/null 2>&1; then
            [ -f "${DASHBOARD_BIN}" ] || echo "警告: 看板构建报告成功但找不到 ${DASHBOARD_BIN}——本次不含看板"
        else
            echo "警告: Qt 看板编译失败——本次发布不含看板（业务链路不受影响）"
        fi

        rm -rf dist-arm64
        mkdir -p dist-arm64/server/bin dist-arm64/server/build dist-arm64/server/test \
                 dist-arm64/client/bin dist-arm64/client/lib dist-arm64/lib

        install -m 0755 build-arm64-native/server/weaknet-dbus-server dist-arm64/server/bin/
        install -m 0755 build-arm64-native/server/history_query_tool dist-arm64/server/bin/
        install -m 0755 build-arm64-native/server/test/test_ebpf dist-arm64/server/test/
        install -m 0755 build-arm64-native/server/test/test_crash_handler dist-arm64/server/test/
        for f in build-arm64-native/server/build/*.bpf.o; do install -m 0644 "$f" dist-arm64/server/build/ 2>/dev/null || true; done
        install -m 0755 build-arm64-native/client/bin/test_client_bin dist-arm64/client/bin/test-client
        install -m 0755 build-arm64-native/client/bin/weaknet_cli dist-arm64/client/bin/weaknet-cli
        install -m 0755 build-arm64-native/client/bin/weaknet_health_guard dist-arm64/client/bin/weaknet-health-guard
        install -m 0755 client/weaknet-offline-diag dist-arm64/client/bin/weaknet-offline-diag
        # 带 SONAME 的库：cp -a 保留 libweaknet.so.1.1.0 + 两条软链。
        # 用 `install libweaknet.so` 只会落一个同名真文件，而消费方 DT_NEEDED
        # 是 `libweaknet.so.1`，板上必然加载失败。
        cp -a build-arm64-native/client/lib/libweaknet.so* dist-arm64/client/lib/
        if [ -f "${DASHBOARD_BIN}" ]; then
            install -m 0755 "${DASHBOARD_BIN}" dist-arm64/client/bin/weaknet-dashboard
        fi
        install -m 0755 "AI-assisted analysis/edge_network_rag.py" dist-arm64/edge_network_rag.py
        install -m 0644 "AI-assisted analysis/network_knowledge_base.py" dist-arm64/network_knowledge_base.py
        cp -a "${SYSROOT_DIR}/usr/local/lib/libbpf.so"* dist-arm64/lib/ 2>/dev/null || true

        install -m 0644 "${ROOT}/VERSION" dist-arm64/VERSION
        install -m 0755 "${ROOT}/tools/weaknet-local-update.sh" dist-arm64/weaknet-local-update.sh

        # ===== 内核模块（观测面）=====

        # 缺陷背景（C2）：此前 release.sh 完全不打 kmod，发布出去的固件没有

        # weaknet_cpu_stat.ko / weaknet_softirq.ko，`releases/<ver>/kmod/` 为空，

        # weaknet-kmod.service 必然失败 → 板上 host_pressure 恒 unavailable。

        # 观测能力在「发布路径」上静默丢失，而这正是运维最需要的归因数据。

        #

        # 与 ci.sh 一致：失败**不中断**发布。模块与用户态消费者共享

        # kmod/weaknet_kmod_abi.h，ABI 版本必须同源发布；但缺它业务照常运行

        # （host_pressure 报 unavailable），不该让整次发布卡住。

        if "${ROOT}/tools/kmod_cross_build.sh" --out dist-arm64/kmod >/dev/null 2>&1; then
            echo "kmod 产物: $(ls -1 dist-arm64/kmod/*.ko 2>/dev/null | wc -l) 个"
        else
            echo "警告: kmod 交叉编译失败——本次发布不含内核模块（host_pressure 将报 unavailable，业务不受影响）"
            rm -rf dist-arm64/kmod
        fi
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

# 临时复制 dist 目录以准备发布包。
#
# 必须用 `cp -a`（保留软链），不能用 `cp -rL`（解引用）：dist-arm64 里
# `libweaknet.so` / `libweaknet.so.1` → `libweaknet.so.1.1.0`、`libbpf.so`
# / `libbpf.so.1` → `libbpf.so.1.3.0` 是 SONAME 链，板端 adopt 后也是
# 同样的软链形态。用 -L 会把每个软链摊成一份完整副本：包从 5 MB 涨到
# 12 MB（重复 3 份 libweaknet + 3 份 libbpf），且制品布局与板端 release
# 目录不一致 —— 制品就不再能如实复现现场。
#
# `package` 里的 manifest 用 `find . -type f`，本就跳过软链；tar 默认保留
# 软链；板端 `tar --no-same-owner -xf` 也保留。整条链不依赖 -L。
TEMP_DIST="$(mktemp -d /tmp/wn-rel-dist-XXXXXX)"
cp -a "${ROOT}/dist-arm64/." "${TEMP_DIST}/"

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
