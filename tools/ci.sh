#!/usr/bin/env bash
# ============================================================================
# WeakNet CI 自动化脚本（唯一入口）
#
# 编译 → 打包 → 部署 → 测试
#
# 用法:
#   ./tools/ci.sh                    # 日常：编译+部署+测试
#   ./tools/ci.sh --commit           # 归档后：commit+push+编译+部署+测试
#   ./tools/ci.sh --local-only       # 只编译不部署
#   ./tools/ci.sh --skip-push        # 不 push
#   ./tools/ci.sh --skip-deploy      # 不部署
#   ./tools/ci.sh --skip-test        # 不测试
#
# 环境变量:
#   CONTAINER - ARM64 构建容器名（默认: weaknet-arm64-dev）
#   BOARD     - 开发板 SSH 地址（默认: radxa@192.168.2.77）
# ============================================================================
set -euo pipefail

# ---- 配置 ----
CONTAINER="${CONTAINER:-weaknet-arm64-dev}"
BOARD="${BOARD:-radxa@192.168.2.77}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIST_DIR="${ROOT}/dist-arm64"
REPORT_DIR="${ROOT}/ci-reports"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

# ---- 参数解析 ----
DO_COMMIT=false
LOCAL_ONLY=false
SKIP_PUSH=false
SKIP_DEPLOY=false
SKIP_TEST=false
for arg in "$@"; do
    case $arg in
        --commit) DO_COMMIT=true ;;
        --local-only) LOCAL_ONLY=true; SKIP_PUSH=true; SKIP_DEPLOY=true; SKIP_TEST=true ;;
        --skip-push) SKIP_PUSH=true ;;
        --skip-deploy) SKIP_DEPLOY=true ;;
        --skip-test) SKIP_TEST=true ;;
        --help|-h)
            echo "用法: $0 [--commit] [--local-only] [--skip-push] [--skip-deploy] [--skip-test]"
            echo ""
            echo "  --commit      git commit + push（归档后使用）"
            echo "  --local-only  只编译不部署"
            echo "  --skip-push   不 push"
            echo "  --skip-deploy 不部署到开发板"
            echo "  --skip-test   不运行开发板测试"
            echo ""
            echo "环境变量:"
            echo "  CONTAINER=容器名   ARM64 构建容器（默认: weaknet-arm64-dev）"
            echo "  BOARD=用户名@IP    开发板地址（默认: radxa@192.168.2.77）"
            exit 0
            ;;
        *) echo "未知参数: $arg"; exit 1 ;;
    esac
done

# ---- 颜色 ----
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
pass() { echo -e "${GREEN}✅ $1${NC}"; }
fail() { echo -e "${RED}❌ $1${NC}"; exit 1; }
info() { echo -e "${CYAN}ℹ️  $1${NC}"; }
warn() { echo -e "${YELLOW}⚠️  $1${NC}"; }

# ---- 创建报告目录 ----
mkdir -p "${REPORT_DIR}"
REPORT="${REPORT_DIR}/ci_${TIMESTAMP}.txt"
log() { echo "$1" | tee -a "$REPORT"; }

# ============================================================
log "=============================================="
log "  WeakNet CI — $(date)"
log "=============================================="

# ============================================================
# Step 1: Git commit + push（可选）
# ============================================================
if [ "$DO_COMMIT" = true ]; then
    log ""
    log "===== Step 1: Git 提交 ====="

    if [ -n "$(git status --porcelain)" ]; then
        cd "$ROOT"
        git add -A
        git commit -m "chore: harness 归档后全量提交

$(git diff --cached --stat | tail -1)"
        pass "提交完成"
    else
        info "工作区干净，无需提交"
    fi

    if [ "$SKIP_PUSH" = false ]; then
        BRANCH=$(git branch --show-current)
        cd "$ROOT"
        git push origin "$BRANCH" && pass "推送成功" || fail "推送失败"
    else
        info "跳过推送"
    fi
fi

# ============================================================
# Step 2: ARM64 编译 + 打包（增量编译）
# ============================================================
log ""
log "===== Step 2: ARM64 编译 + 打包 ====="

docker exec "${CONTAINER}" bash -c '
set -euo pipefail
cd /src

# 增量编译：保留 build 目录，仅重建改动的文件
# 使用 ccache 缓存编译结果，减少重复编译
 echo "--- CMake 配置（增量 + ccache）---"
cmake -B build-arm64 -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache \
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache 2>&1

echo "--- 编译服务端 + eBPF + 客户端 ---"
cmake --build build-arm64 --target weaknet-dbus-server history_query_tool weaknet test_client_bin ebpf -j1 2>&1

echo "--- 打包 dist-arm64 ---"
rm -rf dist-arm64
mkdir -p dist-arm64/server/bin dist-arm64/server/build \
         dist-arm64/client/bin dist-arm64/client/lib dist-arm64/lib

install -m 0755 build-arm64/server/weaknet-dbus-server dist-arm64/server/bin/
install -m 0755 build-arm64/server/history_query_tool dist-arm64/server/bin/
for f in build-arm64/server/build/*.bpf.o; do install -m 0644 "$f" dist-arm64/server/build/ 2>/dev/null || true; done
install -m 0755 build-arm64/client/bin/test_client_bin dist-arm64/client/bin/test-client
install -m 0644 build-arm64/client/lib/libweaknet.so dist-arm64/client/lib/
cp -a /usr/local/lib/libbpf.so* dist-arm64/lib/ 2>/dev/null || true

echo "产物: $(find dist-arm64 -type f | wc -l) 个文件"
file dist-arm64/server/bin/weaknet-dbus-server

# 输出 ccache 命中率（统计命令失败不应影响编译/打包成功）
echo "--- ccache 统计 ---"
if command -v ccache >/dev/null 2>&1; then
    ccache -s 2>/dev/null | grep -E "cache hit|cache miss|hit rate|files in cache|cache size" || true
else
    echo "ccache 未安装，跳过统计"
fi' 2>&1 | tee -a "$REPORT"

pass "编译 + 打包完成"

if [ "$LOCAL_ONLY" = true ]; then
    log ""
    pass "本地编译完成（跳过部署）"
    exit 0
fi

# ============================================================
# Step 3: 部署到开发板
# ============================================================
if [ "$SKIP_DEPLOY" = false ]; then
    log ""
    log "===== Step 3: 部署到开发板 ====="

    if ! ping -c 1 -W 2 "${BOARD#*@}" >/dev/null 2>&1; then
        fail "开发板不可达"
    fi

    # 清理板上源码、构建中间文件和旧部署残留；保留持久化 data/ 数据
    ssh "${BOARD}" "sudo systemctl stop weaknet-server 2>/dev/null || true; sudo killall weaknet-dbus-server 2>/dev/null || true; sudo rm -rf /home/radxa/weaknet/server/src /home/radxa/weaknet/server/include /home/radxa/weaknet/build /home/radxa/weaknet/server/build /home/radxa/weaknet/client/src /home/radxa/weaknet/CMakeFiles /home/radxa/weaknet/logs /home/radxa/weaknet/server/server 2>/dev/null || true"

    # 只同步 dist-arm64 编译产物，不同步源码和构建目录
    rsync -az --delete --exclude "data/" --exclude "server/logs/" -e ssh "${DIST_DIR}/" "${BOARD}:/home/radxa/weaknet/" 2>/dev/null

    # 部署 systemd 单元并启动唯一服务实例
    scp "${ROOT}/tools/weaknet-server.service" "${BOARD}:/tmp/weaknet-server.service" 2>/dev/null
    ssh "${BOARD}" "sudo cp /tmp/weaknet-server.service /etc/systemd/system/weaknet-server.service && sudo systemctl daemon-reload && sudo systemctl enable weaknet-server && sudo systemctl restart weaknet-server"
    pass "部署完成"
else
    info "跳过部署"
fi

# ============================================================
# Step 4: 开发板测试
# ============================================================
if [ "$SKIP_TEST" = false ]; then
    log ""
    log "===== Step 4: 开发板测试 ====="

    # 上传测试脚本
    scp "${ROOT}/tools/weaknet-test-full.sh" "${BOARD}:/home/radxa/weaknet/weaknet-test-full.sh" 2>/dev/null
    ssh "${BOARD}" "chmod +x /home/radxa/weaknet/weaknet-test-full.sh" 2>/dev/null

    # 运行测试
    FUNC_RESULT=$(ssh -t "${BOARD}" "sudo /home/radxa/weaknet/weaknet-test-full.sh" 2>&1 | head -80) || true
    echo "$FUNC_RESULT" | tee -a "$REPORT"

    # 解析结果
    HEALTH_JSON=$(echo "$FUNC_RESULT" | grep "健康检查结果" | head -1 | sed 's/.*: //')
    if [ -n "$HEALTH_JSON" ]; then
        log ""
        log "功能测试结果:"
        log "  接口: $(echo "$HEALTH_JSON" | grep -o '"interface":"[^"]*"' | cut -d'"' -f4)"
        log "  质量分数: $(echo "$HEALTH_JSON" | grep -o '"quality_score":[0-9.]*' | cut -d: -f2)"
        log "  RTT: $(echo "$HEALTH_JSON" | grep -o '"rtt_ms":[0-9-]*' | cut -d: -f2) ms"
        log "  RSSI: $(echo "$HEALTH_JSON" | grep -o '"rssi_dbm":[0-9-]*' | cut -d: -f2) dBm"
    fi

    pass "测试完成"
else
    info "跳过测试"
fi

# ============================================================
log ""
log "=============================================="
pass "全部完成"
log "=============================================="
