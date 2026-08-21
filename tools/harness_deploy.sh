#!/usr/bin/env bash
# ============================================================================
# harness_deploy.sh - 结合 harness 流程的一键部署脚本
#
# 在 harness 归档 (change_archive.sh) 完成后运行，自动完成：
#   1. git commit（归档后的全量提交）
#   2. git push（触发 GitHub CI）
#   3. ARM64 容器内编译
#   4. 打包 dist-arm64
#   5. 部署到开发板
#   6. 运行开发板测试
#
# 用法:
#   ./tools/harness_deploy.sh              # 完整流程
#   ./tools/harness_deploy.sh --skip-push  # 跳过 git push（不触发远程 CI）
#   ./tools/harness_deploy.sh --skip-test  # 跳过开发板测试
#   ./tools/harness_deploy.sh --local-only # 只做本地编译，不部署
# ============================================================================
set -euo pipefail

CONTAINER="${CONTAINER:-weaknet-arm64-dev}"
BOARD="${BOARD:-radxa@192.168.2.77}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

SKIP_PUSH=false
SKIP_DEPLOY=false
SKIP_TEST=false
LOCAL_ONLY=false

for arg in "$@"; do
    case $arg in
        --skip-push) SKIP_PUSH=true ;;
        --skip-deploy) SKIP_DEPLOY=true ;;
        --skip-test) SKIP_TEST=true ;;
        --local-only) LOCAL_ONLY=true; SKIP_PUSH=true; SKIP_DEPLOY=true; SKIP_TEST=true ;;
        --help|-h)
            echo "用法: $0 [--skip-push] [--skip-deploy] [--skip-test] [--local-only]"
            exit 0
            ;;
    esac done

RED='\033[0;31m'; GREEN='\033[0;32m'; CYAN='\033[0;36m'; NC='\033[0m'
pass() { echo -e "${GREEN}✅ $1${NC}"; }
fail() { echo -e "${RED}❌ $1${NC}"; exit 1; }
info() { echo -e "${CYAN}ℹ️  $1${NC}"; }

# ============================================================
echo "=============================================="
echo "  Harness 部署流程 — $(date)"
echo "=============================================="

# ---- Step 1: Git commit（归档后全量提交）----
echo ""
info "Step 1: Git 提交"
if [ -n "$(git status --porcelain)" ]; then
    git add -A
    git commit -m "chore: harness 归档后全量提交

$(git diff --cached --stat | tail -1)"
    pass "提交完成"
else
    info "工作区干净，无需提交"
fi

# ---- Step 2: Git push（触发远程 CI）----
echo ""
info "Step 2: Git 推送"
if [ "$SKIP_PUSH" = false ]; then
    BRANCH=$(git branch --show-current)
    git push origin "$BRANCH" 2>&1 && pass "推送成功，GitHub CI 已触发" || fail "推送失败"
else
    info "跳过推送"
fi

# ---- Step 3: ARM64 编译 ----
echo ""
info "Step 3: ARM64 容器内编译"
docker exec "$CONTAINER" bash -c '
set -euo pipefail
cd /src
cmake -B build-cmake -DCMAKE_BUILD_TYPE=Debug 2>&1 | tail -3
cmake --build build-cmake --target weaknet-dbus-server history_query_tool weaknet test_client_bin -j1 2>&1 | tail -5
' && pass "编译完成" || fail "编译失败"

# ---- Step 4: 打包 ----
echo ""
info "Step 4: 打包 dist-arm64"
docker exec "$CONTAINER" bash -c '
cd /src
rm -rf dist-arm64
mkdir -p dist-arm64/server/bin dist-arm64/server/build \
         dist-arm64/client/bin dist-arm64/client/lib dist-arm64/lib
install -m 0755 build-cmake/server/weaknet-dbus-server dist-arm64/server/bin/
install -m 0755 build-cmake/server/history_query_tool dist-arm64/server/bin/
for f in build-cmake/server/ebpf/*.bpf.o; do install -m 0644 "$f" dist-arm64/server/build/ 2>/dev/null || true; done
install -m 0755 client/bin/test_client_bin dist-arm64/client/bin/test-client
install -m 0644 client/lib/libweaknet.so dist-arm64/client/lib/
cp -a /usr/local/lib/libbpf.so* dist-arm64/lib/
echo "产物: $(find dist-arm64 -type f | wc -l) 个文件"
' && pass "打包完成" || fail "打包失败"

if [ "$LOCAL_ONLY" = true ]; then
    echo ""
    pass "本地编译完成（跳过部署）"
    exit 0
fi

# ---- Step 5: 部署 ----
echo ""
info "Step 5: 部署到开发板"
if [ "$SKIP_DEPLOY" = true ]; then
    info "跳过部署"
else
    if ! ping -c 1 -W 2 "${BOARD#*@}" >/dev/null 2>&1; then
        fail "开发板不可达"
    fi
    ssh "$BOARD" "sudo rm -rf /home/radxa/weaknet/logs /home/radxa/weaknet/server/server 2>/dev/null || true"
    rsync -az --delete --exclude 'server/logs/' -e ssh dist-arm64/ "$BOARD:/home/radxa/weaknet/" && pass "部署完成" || fail "部署失败"
fi

# ---- Step 6: 测试 ----
echo ""
info "Step 6: 开发板测试"
if [ "$SKIP_TEST" = true ]; then
    info "跳过测试"
else
    ssh -t "$BOARD" "sudo /home/radxa/weaknet-test-full.sh" 2>&1 | tail -20 && pass "测试完成" || fail "测试失败"
fi

echo ""
echo "=============================================="
pass "全部完成"
echo "=============================================="
