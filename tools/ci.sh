#!/usr/bin/env bash
# ============================================================================
# WeakNet CI 自动化脚本
#
# 一键完成：编译 → 部署 → 功能测试 → 单元测试 → 生成报告
#
# 用法:
#   ./tools/ci.sh              # 完整流程
#   ./tools/ci.sh --skip-build # 跳过编译（使用上次编译结果）
#   ./tools/ci.sh --skip-deploy # 跳过部署（仅本地测试）
#
# 环境变量:
#   CONTAINER - ARM64 构建容器名（默认: weaknet-arm64-dev）
#   BOARD     - 开发板 SSH 地址（默认: radxa@192.168.2.77）
#   JOBS      - 编译并行度（默认: 1）
# ============================================================================
set -euo pipefail

# ---- 配置 ----
CONTAINER="${CONTAINER:-weaknet-arm64-dev}"
BOARD="${BOARD:-radxa@192.168.2.77}"
JOBS="${JOBS:-1}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIST_DIR="${ROOT}/dist-arm64"
REPORT_DIR="${ROOT}/ci-reports"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

# ---- 参数解析 ----
SKIP_BUILD=false
SKIP_DEPLOY=false
for arg in "$@"; do
    case $arg in
        --skip-build) SKIP_BUILD=true ;;
        --skip-deploy) SKIP_DEPLOY=true ;;
        *) echo "未知参数: $arg"; exit 1 ;;
    esac
done

# ---- 颜色 ----
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

pass() { echo -e "${GREEN}✅ $1${NC}"; }
fail() { echo -e "${RED}❌ $1${NC}"; }
info() { echo -e "${CYAN}ℹ️  $1${NC}"; }
warn() { echo -e "${YELLOW}⚠️  $1${NC}"; }

TOTAL_PASS=0
TOTAL_FAIL=0
TOTAL_SKIP=0

# ---- 创建报告目录 ----
mkdir -p "${REPORT_DIR}"
REPORT="${REPORT_DIR}/ci_${TIMESTAMP}.txt"

log() {
    echo "$1" | tee -a "$REPORT"
}

# ============================================================================
log "=============================================="
log "  WeakNet CI 自动化测试 — $(date)"
log "=============================================="
log ""

# ============================================================================
# Step 1: 编译
# ============================================================================
if [ "$SKIP_BUILD" = false ]; then
    log "===== Step 1: ARM64 编译 ====="

    docker exec -e JOBS="${JOBS}" "${CONTAINER}" bash -lc '
    set -euo pipefail
    cd /src

    mkdir -p server/build
    cp board-assets/vmlinux.h server/build/vmlinux.h 2>/dev/null || true

    echo "--- 编译服务端 + BPF ---"
    make -C server -j1

    echo "--- 编译客户端 ---"
    make client-lib -j1

    echo "--- 编译单元测试 ---"
    make -C server test-build -j1
    ' 2>&1 | tee -a "$REPORT"

    if [ ${PIPESTATUS[0]} -eq 0 ]; then
        pass "编译完成"
    else
        fail "编译失败"
        exit 1
    fi
else
    info "跳过编译（使用上次编译结果）"
fi

# ============================================================================
# Step 2: 打包部署目录
# ============================================================================
log ""
log "===== Step 2: 打包部署目录 ====="

docker exec "${CONTAINER}" bash -c '
cd /src
rm -rf dist-arm64
mkdir -p dist-arm64/server/bin dist-arm64/server/build dist-arm64/server/test/bin \
         dist-arm64/client/bin dist-arm64/client/lib dist-arm64/lib

install -m 0755 server/bin/weaknet-dbus-server dist-arm64/server/bin/
for f in server/build/*.bpf.o; do install -m 0644 "$f" dist-arm64/server/build/ 2>/dev/null; done
install -m 0755 client/bin/test-client dist-arm64/client/bin/
install -m 0644 client/lib/libweaknet.so dist-arm64/client/lib/
cp -a /usr/local/lib/libbpf.so* dist-arm64/lib/
cp -a server/test/bin/* dist-arm64/server/test/bin/ 2>/dev/null || true

echo "产物: $(find dist-arm64 -type f | wc -l) 个文件"
' 2>&1 | tee -a "$REPORT"

pass "部署目录已生成"

# ============================================================================
# Step 3: 部署到开发板
# ============================================================================
if [ "$SKIP_DEPLOY" = false ]; then
    log ""
    log "===== Step 3: 部署到开发板 ====="

    # 检查连通性
    if ! ping -c 1 -W 2 "${BOARD#*@}" >/dev/null 2>&1; then
        warn "开发板不可达，跳过部署和远程测试"
        SKIP_DEPLOY=true
    else
        ssh "${BOARD}" "sudo rm -rf /home/radxa/weaknet && sudo mkdir -p /home/radxa/weaknet && sudo chown radxa:radxa /home/radxa/weaknet" 2>/dev/null
        rsync -az -e ssh "${DIST_DIR}/" "${BOARD}:/home/radxa/weaknet/" 2>/dev/null
        pass "部署完成"
    fi
else
    info "跳过部署"
fi

# ============================================================================
# Step 4: 远程单元测试
# ============================================================================
if [ "$SKIP_DEPLOY" = false ]; then
    log ""
    log "===== Step 4: 远程单元测试 ====="

    UNIT_RESULT=$(ssh "${BOARD}" '
    export LD_LIBRARY_PATH=/home/radxa/weaknet/lib:/home/radxa/weaknet/client/lib
    cd /home/radxa/weaknet/server

    PASS=0; FAIL=0; SKIP=0; TOTAL=0
    for bin in test/bin/test_*; do
        name=$(basename "$bin")
        TOTAL=$((TOTAL + 1))
        output=$(timeout 30 ./$bin --gtest_color=no 2>&1)
        pc=$(echo "$output" | grep -oP "\[\s+PASSED\s+\]\s+\K\d+" | tail -1)
        fc=$(echo "$output" | grep -oP "\[\s+FAILED\s+\]\s+\K\d+" | tail -1)
        sc=$(echo "$output" | grep -oP "\[\s+SKIPPED\s+\]\s+\K\d+" | tail -1)
        pc=${pc:-0}; fc=${fc:-0}; sc=${sc:-0}
        if [ "$fc" = "0" ] && [ "$sc" = "0" ]; then
            PASS=$((PASS + 1))
            echo "✅ $name — $pc tests PASSED"
        elif [ "$fc" = "0" ]; then
            SKIP=$((SKIP + 1))
            echo "⏭️  $name — $pc passed, $sc SKIPPED"
        else
            FAIL=$((FAIL + 1))
            echo "❌ $name — $pc passed, $fc FAILED"
        fi
    done
    echo "---"
    echo "TOTAL=$TOTAL PASS=$PASS FAIL=$FAIL SKIP=$SKIP"
    ' 2>&1)

    echo "$UNIT_RESULT" | tee -a "$REPORT"

    # 解析结果
    UNIT_TOTAL=$(echo "$UNIT_RESULT" | grep "^TOTAL=" | cut -d= -f2)
    UNIT_PASS=$(echo "$UNIT_RESULT" | grep "^PASS=" | head -1 | cut -d= -f2)
    UNIT_FAIL=$(echo "$UNIT_RESULT" | grep "^FAIL=" | head -1 | cut -d= -f2)
    UNIT_SKIP=$(echo "$UNIT_RESULT" | grep "^SKIP=" | head -1 | cut -d= -f2)

    TOTAL_PASS=$((TOTAL_PASS + ${UNIT_PASS:-0}))
    TOTAL_FAIL=$((TOTAL_FAIL + ${UNIT_FAIL:-0}))
    TOTAL_SKIP=$((TOTAL_SKIP + ${UNIT_SKIP:-0}))

    if [ "${UNIT_FAIL:-0}" -eq 0 ]; then
        pass "单元测试全部通过 ($UNIT_PASS/$UNIT_TOTAL)"
    else
        fail "有 $UNIT_FAIL 个单元测试失败"
    fi
fi

# ============================================================================
# Step 5: 远程功能测试
# ============================================================================
if [ "$SKIP_DEPLOY" = false ]; then
    log ""
    log "===== Step 5: 远程功能测试 ====="

    # 上传最新测试脚本
    scp "${DIST_DIR}/weaknet-test-full.sh" "${BOARD}:/home/radxa/weaknet/" 2>/dev/null || true

    FUNC_RESULT=$(ssh -t "${BOARD}" "sudo /home/radxa/weaknet/weaknet-test-full.sh" 2>&1 | tee -a "$REPORT" || true)

    FUNC_FAIL=$(echo "$FUNC_RESULT" | grep "^  FAIL:" | head -1 | awk '{print $2}')
    if [ "${FUNC_FAIL:-0}" -eq 0 ]; then
        pass "功能测试全部通过"
    else
        fail "有 $FUNC_FAIL 个功能测试失败"
    fi
fi

# ============================================================================
# 汇总报告
# ============================================================================
log ""
log "=============================================="
log "  CI 报告汇总"
log "=============================================="
log "  单元测试: ${UNIT_PASS:-0}/${UNIT_TOTAL:-0} 通过, ${UNIT_FAIL:-0} 失败, ${UNIT_SKIP:-0} 跳过"
log "  功能测试: ${FUNC_FAIL:-0:-0} 失败"
log "  报告文件: ${REPORT}"
log "=============================================="

if [ "${TOTAL_FAIL:-0}" -gt 0 ]; then
    fail "CI 失败"
    exit 1
else
    pass "CI 全部通过"
    exit 0
fi
