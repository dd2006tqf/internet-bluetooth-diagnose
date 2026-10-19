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
#   SYSROOT_DIR - ARM64 Sysroot 路径（默认: /opt/sysroot-bullseye-arm64）
#   BOARD       - 开发板 SSH 地址（默认: radxa@radxa-cubie-a7a.local 或 board）
# ============================================================================
set -euo pipefail

# ---- 配置 ----
SYSROOT_DIR="${SYSROOT_DIR:-/opt/sysroot-bullseye-arm64}"
BOARD="${BOARD:-radxa@radxa-cubie-a7a.local}"
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
            echo "  SYSROOT_DIR=路径   ARM64 Bullseye Sysroot（默认: /opt/sysroot-bullseye-arm64）"
            echo "  BOARD=主机或别名   开发板地址（默认: radxa@radxa-cubie-a7a.local 或 board）"
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
        # S9 残留修复：本机常有多个会话共享同一 worktree，`git add -A` 会把
        # 其它会话的未跟踪文件（WIP 源码、临时二进制、VERSION 之类）一并扫进
        # 归档提交。改为两级：-u 只暂存已跟踪文件的改动/删除；新增文件仅放行
        # openspec/（归档把 changes/<name> 移动为 archive/<name> 时产生的新
        # 路径都在其下，含 ai_snapshot.json 之外的全部归档产物）。
        git add -u
        git add openspec/
        if ! git diff --cached --quiet; then
            git commit -m "chore: harness 归档后全量提交

$(git diff --cached --stat | tail -1)"
            pass "提交完成"
        else
            info "无已跟踪改动或 openspec 新增文件，无需提交"
        fi
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
# Step 2: ARM64 编译 + 打包（宿主机原生交叉编译）
# ============================================================
log ""
log "===== Step 2: ARM64 编译 + 打包 ====="

# 使用宿主机裸机原生交叉编译（/opt/sysroot-bullseye-arm64 + clang -j$(nproc)），
# 完全脱离 QEMU 模拟与 Docker 容器，耗时从 ~15 分钟降至 ~40 秒。
BUILD_STATUS=0
if [ ! -d "${SYSROOT_DIR}" ] || [ ! -f "${ROOT}/cmake/toolchain-arm64-cross.cmake" ]; then
    fail "缺少 Sysroot (${SYSROOT_DIR}) 或工具链文件。请先运行 sudo ./scripts/setup_arm64_sysroot.sh 初始化"
fi

info "使用宿主机裸机原生交叉编译 (Clang + Bullseye Sysroot, -j$(nproc))"
(
    set -euo pipefail
    cd "${ROOT}"

    echo "--- CMake 配置（宿主机原生交叉编译）---"
    cmake -B build-arm64-native \
          -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-arm64-cross.cmake \
          -DCMAKE_BUILD_TYPE=Debug \
          -DBUILD_EBPF=ON 2>&1

    echo "--- 编译服务端 + eBPF + 客户端 (-j$(nproc)) ---"
    cmake --build build-arm64-native --target weaknet-dbus-server history_query_tool weaknet weaknet_cli test_client_bin test_ebpf ebpf -j"$(nproc)" 2>&1

    echo "--- 打包 dist-arm64 ---"
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

    echo "产物: $(find dist-arm64 -type f | wc -l) 个文件"
    file dist-arm64/server/bin/weaknet-dbus-server

    echo "--- ccache 统计 ---"
    if command -v ccache >/dev/null 2>&1; then
        ccache -s 2>/dev/null | grep -E "cache hit|cache miss|hit rate|files in cache|cache size" || true
    fi
) 2>&1 | tee -a "$REPORT" || BUILD_STATUS=${PIPESTATUS[0]}

if [ "$BUILD_STATUS" -ne 0 ]; then
    fail "ARM64 编译/打包失败（退出码 ${BUILD_STATUS}）——中止：不打印成功、不进入部署，板端保持旧版本"
fi
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

    if [ "${BOARD}" != "board" ] && ! ping -c 1 -W 2 "${BOARD#*@}" >/dev/null 2>&1; then
        fail "开发板不可达"
    fi

    # 清理板上源码、构建中间文件和旧部署残留；保留持久化 data/ 数据
    # __pycache__ 必须以 root 删除：root 进程（诊断 RAG 脚本等）会生成 root 属主的
    # .pyc，radxa 用户无法 unlink，导致后续 rsync --delete 恒失败（实测踩过 3 次）
    ssh "${BOARD}" "sudo systemctl stop weaknet-server 2>/dev/null || true; sudo killall weaknet-dbus-server 2>/dev/null || true; sudo rm -rf /home/radxa/weaknet/__pycache__ /home/radxa/weaknet/server/src /home/radxa/weaknet/server/include /home/radxa/weaknet/build /home/radxa/weaknet/server/build /home/radxa/weaknet/client/src /home/radxa/weaknet/CMakeFiles /home/radxa/weaknet/logs /home/radxa/weaknet/server/server 2>/dev/null || true"

    # 只同步 dist-arm64 编译产物，不同步源码和构建目录（保留持久化 data/、config/ 密钥及日志）
    rsync -az --delete --exclude "data/" --exclude "config/" --exclude "server/logs/" -e ssh "${DIST_DIR}/" "${BOARD}:/home/radxa/weaknet/" \
        || fail "rsync 部署失败——板端可能仍是旧产物"

    # 旧 web-dashboard 已废弃删除：前端统一由 Large-Model-Application（Next.js）承载，
    # 开发板只保留 eBPF + C++ 底座。此处顺带停用并清理板上遗留的 weaknet-web 服务，
    # 避免旧 unit 继续拉起已失去部署来源的旧网关。
    ssh "${BOARD}" "sudo systemctl disable --now weaknet-web 2>/dev/null || true; \
        sudo rm -f /etc/systemd/system/weaknet-web.service 2>/dev/null || true; \
        sudo rm -rf /home/radxa/weaknet/web 2>/dev/null || true; \
        sudo systemctl daemon-reload 2>/dev/null || true"

    # 部署 systemd 单元、D-Bus 系统总线策略、运行时配置并启动服务实例
    scp "${ROOT}/tools/weaknet-server.service" "${BOARD}:/tmp/weaknet-server.service" 2>/dev/null
    scp "${ROOT}/tools/com.example.WeakNet.conf" "${BOARD}:/tmp/com.example.WeakNet.conf" 2>/dev/null
    # S5：Edge 预共享令牌通过安全通道渲染并直接安装到 /etc/weaknet/config.yaml（0600），
    # 绝不写入 /tmp（消除世界可读窗口），且 token 绝不进入远程命令 argv（ps 可见风险）。
    EDGE_TOKEN_FILE="${ROOT}/secrets/edge_token"
    [[ -s "${EDGE_TOKEN_FILE}" ]] || fail "缺少 ${EDGE_TOKEN_FILE}（edge 上行预共享令牌），先创建该文件再部署"
    EDGE_TOKEN="$(tr -d '[:space:]' < "${EDGE_TOKEN_FILE}")"
    [[ "${EDGE_TOKEN}" =~ ^[A-Za-z0-9_-]+$ ]] || fail "secrets/edge_token 含非法字符（仅允许 [A-Za-z0-9_-]）"
    RENDERED_CONFIG="$(mktemp "${ROOT}/.ci-config-XXXXXX.yaml")"
    chmod 0600 "${RENDERED_CONFIG}"
    sed "s|^  token: \"\"$|  token: \"${EDGE_TOKEN}\"|" config.yaml > "${RENDERED_CONFIG}"
    ssh "${BOARD}" "sudo mkdir -p /etc/weaknet && sudo install -m 0600 /dev/stdin /etc/weaknet/config.yaml" < "${RENDERED_CONFIG}" || {
        shred -u "${RENDERED_CONFIG}" 2>/dev/null || rm -f "${RENDERED_CONFIG}"
        fail "edge token 注入板端失败（stdin → /etc/weaknet/config.yaml）"
    }
    shred -u "${RENDERED_CONFIG}" 2>/dev/null || rm -f "${RENDERED_CONFIG}"
    pass "edge token 已安全注入 /etc/weaknet/config.yaml（0600，不进 argv/tmp）"

    # 客户端动态库安装到系统路径，供 weaknet-cli 链接（服务端走 unit 内 LD_LIBRARY_PATH）
    scp "${DIST_DIR}/client/lib/libweaknet.so" "${BOARD}:/tmp/libweaknet.so" 2>/dev/null
    ssh "${BOARD}" "sudo cp /tmp/weaknet-server.service /etc/systemd/system/weaknet-server.service && \
        sudo cp /tmp/com.example.WeakNet.conf /etc/dbus-1/system.d/com.example.WeakNet.conf && \
        sudo cp /tmp/libweaknet.so /usr/local/lib/libweaknet.so && sudo ldconfig && \
        sudo chmod o+rx /home/radxa/weaknet/data 2>/dev/null || true && \
        sudo systemctl daemon-reload && sudo systemctl reload dbus 2>/dev/null || true; \
        sudo systemctl enable weaknet-server && sudo systemctl restart weaknet-server"

    scp "${ROOT}/tools/weaknet-test-full.sh" "${BOARD}:/home/radxa/weaknet/weaknet-test-full.sh" 2>/dev/null
    ssh "${BOARD}" "chmod +x /home/radxa/weaknet/weaknet-test-full.sh" 2>/dev/null
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

    # 运行测试（weaknet-test-full.sh 以 FAIL 条数作为退出码）
    #  - 完整输出存报告，控制台只显示前 80 行（截断只是显示，不再是取证上限）
    #  - 退出码必须捕获：旧写法 `|| true` 会吞掉失败，然后无条件打印
    #    "测试完成"，冒烟挂了 CI 也报成功——与构建失败同级的发布可信度缺陷
    FUNC_RESULT=""
    TEST_EXIT=0
    FUNC_RESULT=$(ssh -t "${BOARD}" "sudo /home/radxa/weaknet/weaknet-test-full.sh" 2>&1) || TEST_EXIT=$?
    printf '%s\n' "$FUNC_RESULT" >> "$REPORT"
    # head 提前退出会让 printf 吃 SIGPIPE(141)；pipefail + set -e 会因此误报失败，
    # 所以显示用的管道必须放行（报告里已有完整输出，截断只是显示）
    printf '%s\n' "$FUNC_RESULT" | head -80 || true

    # 解析结果
    # 显示用解析：grep 无匹配时不能让 set -e 把已通过的测试判成失败
    HEALTH_JSON=$(echo "$FUNC_RESULT" | grep "健康检查结果" | head -1 | sed 's/.*: //' || true)
    if [ -n "$HEALTH_JSON" ]; then
        log ""
        log "功能测试结果:"
        log "  接口: $(echo "$HEALTH_JSON" | grep -o '"interface":"[^"]*"' | cut -d'"' -f4)"
        log "  质量分数: $(echo "$HEALTH_JSON" | grep -o '"quality_score":[0-9.]*' | cut -d: -f2)"
        log "  RTT: $(echo "$HEALTH_JSON" | grep -o '"rtt_ms":[0-9-]*' | cut -d: -f2) ms"
        log "  RSSI: $(echo "$HEALTH_JSON" | grep -o '"rssi_dbm":[0-9-]*' | cut -d: -f2) dBm"
    fi

    # 双保险：退出码 + 输出里的 FAIL 计数
    FAIL_COUNT=$(printf '%s' "$FUNC_RESULT" | grep -oE 'FAIL: [0-9]+' | grep -oE '[0-9]+' | tail -1 || true)
    if [ "${TEST_EXIT}" -ne 0 ] || [ "${FAIL_COUNT:-0}" -gt 0 ]; then
        fail "开发板测试失败（退出码 ${TEST_EXIT}，FAIL=${FAIL_COUNT:-未知}）"
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
