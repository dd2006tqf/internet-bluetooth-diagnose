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

# ---- 版本单一来源（仓库根 VERSION，OTA releases/<version> 与包名都用它） ----
VERSION="$(tr -d '[:space:]' < "${ROOT}/VERSION" 2>/dev/null || echo "")"
if ! [[ "${VERSION}" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    fail "仓库根 VERSION 缺失或非法（需一行 X.Y.Z）：'${VERSION}'"
fi

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

    # OTA 发布物：VERSION 与升级脚本随包下发；manifest.sha256 由板端 adopt
    # 现场按实际落地文件生成（包路径与板端部署内容必然一致）。
    install -m 0644 "${ROOT}/VERSION" dist-arm64/VERSION
    install -m 0755 "${ROOT}/tools/weaknet-local-update.sh" dist-arm64/weaknet-local-update.sh

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

    # 停服 + 清理 __pycache__（root 属主 .pyc，radxa 用户无法 unlink，
    # 曾导致 rsync --delete 恒失败，实测踩过 3 次）。
    # 不再删除旧扁平布局的 server/client 子目录：首次 adopt 会把整个旧布局
    # 归档为 releases/legacy-flat——在兼容软链建立后直接 rm server/build
    # 会经软链误删当前 release 的 .bpf.o（OTA 布局切换期的坑）。
    ssh "${BOARD}" "sudo systemctl stop weaknet-server 2>/dev/null || true; sudo killall weaknet-dbus-server 2>/dev/null || true; sudo rm -rf /home/radxa/weaknet/__pycache__ 2>/dev/null || true"

    # 整个暂存区清空（2026-11-01 完整 ci.sh 实跑暴露的第二个缺陷）。
    #
    # 暂存区是纯粹的**中转目录**：rsync 把 dist-arm64 灌进来，adopt 再把内容
    # 搬进 releases/<ver>，中间没有任何东西需要跨次保留。但历史遗留会在里面
    # 攒下 root 属主文件——服务曾把 .deploy-staging/server 当工作目录写日志，
    # 于是留下 server/server/log、server/logs（Oct 22 的 52MB 日志）。
    # radxa 用户的 rsync --delete 删不掉 root 属主文件，于是**每次部署都恒败**：
    #   rsync: delete_file: unlink(...log) failed: Permission denied (13)
    #   rsync error: some files/attrs were not transferred (code 23)
    # 之前只清了 __pycache__，属于打地鼠；这里直接整目录移除，radxa 重新创建。
    ssh "${BOARD}" "sudo rm -rf /home/radxa/weaknet/.deploy-staging" \
        || warn "暂存区清空失败——若含 root 属主残留，本轮 rsync 可能失败"

    # 只同步 dist-arm64 编译产物到暂存目录（不碰 releases/current/data/config），
    # 由下方 weaknet-local-update.sh adopt 原子迁入 releases/${VERSION}。
    # 旧版遗留的 data/、config/、日志与本次 rsync 彻底解耦（不再依赖 exclude 保护）。
    rsync -az --delete -e ssh "${DIST_DIR}/" "${BOARD}:/home/radxa/weaknet/.deploy-staging/" \
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
    # OTA Phase 2：分发发布公钥到板端 config 目录（供 weaknet-local-update 强制验签）
    if [ -f "${ROOT}/certs/release_pub.pem" ]; then
        scp "${ROOT}/certs/release_pub.pem" "${BOARD}:/tmp/release_pub.pem" 2>/dev/null
        ssh "${BOARD}" "sudo mkdir -p /home/radxa/weaknet/config && sudo cp /tmp/release_pub.pem /home/radxa/weaknet/config/release_pub.pem && sudo chmod 644 /home/radxa/weaknet/config/release_pub.pem"
        pass "发布公钥已同步到板端 config/release_pub.pem"
    fi
    # 注意：weaknet-server.service 本身**不在这里**落盘——它的 Unit 段带
    # `Requires=weaknet-ota-gate.service`，必须等门禁单元先安装（见下方
    # P0-C1 一段），否则 daemon-reload 后即出现"依赖单元不存在"。
    ssh "${BOARD}" "sudo cp /tmp/com.example.WeakNet.conf /etc/dbus-1/system.d/com.example.WeakNet.conf && \
        sudo cp /tmp/libweaknet.so /usr/local/lib/libweaknet.so && sudo ldconfig && \
        sudo chmod o+rx /home/radxa/weaknet/data 2>/dev/null || true && \
        sudo systemctl daemon-reload && sudo systemctl reload dbus 2>/dev/null || true"
    # 注意：此处刻意不 restart——重启由 adopt 在 current 翻转之后执行，
    # 否则新 unit（current/server 路径）在布局未就位时会拉起失败。

    # 反向隧道单元：仓库里有唯一权威版本（tools/weaknet-tunnel.service），
    # 部署时顺手清掉板端历史遗留的 cloud-tunnel.service —— 两个单元都占
    # 板端 -L 8000 端口，后启动者必报 "Address already in use" 反复重启，
    # 云端 2222 反向端口随之失效（表现为 ssh board 全面连不上）。
    #
    # 顺序要求：隧道单元的重启会短暂掐断 SSH 反向通道，本条命令的后半段
    # （adopt → 重启服务 → 健康窗口）都走同一条 SSH 通道，必须放在**隧道
    # 重启之前**完成，否则连接会在通道重建途中被掐断（实测：Connection
    # closed by remote host，后续 adopt 步骤整段没跑，服务停在 inactive）。
    # 所以这里只做"清历史单元 + 落盘新单元"，隧道重启放到本次部署的**收尾**
    # 处（见下方"反向隧道收尾重启"），那里所有不能被掐断的步骤都已经跑完。
    if [ -f "${ROOT}/tools/weaknet-tunnel.service" ]; then
        scp "${ROOT}/tools/weaknet-tunnel.service" "${BOARD}:/tmp/weaknet-tunnel.service" 2>/dev/null
        ssh "${BOARD}" "sudo cp /tmp/weaknet-tunnel.service /etc/systemd/system/weaknet-tunnel.service && \
            sudo systemctl disable --now cloud-tunnel.service 2>/dev/null || true; \
            sudo rm -f /etc/systemd/system/cloud-tunnel.service && \
            sudo systemctl daemon-reload && \
            sudo systemctl enable weaknet-tunnel.service" \
            || warn "反向隧道单元落盘失败（板端 tunnel 可能仍连不上云端）"
        pass "反向隧道单元已落盘（cloud-tunnel 历史单元已清理）"
    fi

    # OTA 事务控制器（P0-A）：装在 $ROOT 之外的系统路径。
    #
    # 为什么不能随 release 走：事务 schema 由它写入，若它随版本升级，
    # 旧版恢复服务读到的就是新版 schema，语义对不上。控制器必须是不随
    # OTA 制品变化的那一份——升级的可靠性不能依赖被升级的东西本身。
    scp "${ROOT}/tools/weaknet-ota-ctl" "${BOARD}:/tmp/weaknet-ota-ctl" 2>/dev/null \
        || fail "OTA 事务控制器上传失败（升级脚本已改为 fail-closed，缺控制器无法升级）"
    ssh "${BOARD}" "sudo install -d -m 0755 /usr/local/lib/weaknet && \
        sudo install -m 0755 /tmp/weaknet-ota-ctl /usr/local/lib/weaknet/weaknet-ota-ctl && \
        sudo install -d -m 0700 /var/lib/weaknet-ota" \
        || fail "OTA 事务控制器安装失败"
    pass "OTA 事务控制器已就位（/usr/local/lib/weaknet/weaknet-ota-ctl）"

    # OTA 开机恢复服务（P0-B）：与控制器同样住在 $ROOT 之外。
    #
    # 恢复服务必须比被恢复的对象更"独立"：它不能等 weaknet-server——
    # 那可能正是起不来的那个。它只读事务状态，做判定与指针操作。
    scp "${ROOT}/tools/weaknet-ota-recover" "${BOARD}:/tmp/weaknet-ota-recover" 2>/dev/null \
        || fail "OTA 恢复服务上传失败"
    scp "${ROOT}/tools/weaknet-ota-recover.service" "${BOARD}:/tmp/weaknet-ota-recover.service" 2>/dev/null \
        || fail "OTA 恢复服务单元上传失败"
    ssh "${BOARD}" "sudo install -m 0755 /tmp/weaknet-ota-recover \
        /usr/local/lib/weaknet/weaknet-ota-recover && \
        sudo cp /tmp/weaknet-ota-recover.service /etc/systemd/system/weaknet-ota-recover.service && \
        sudo systemctl daemon-reload && \
        sudo systemctl enable weaknet-ota-recover.service && \
        sudo systemctl restart weaknet-ota-recover.service" \
        || warn "OTA 恢复服务安装/启动失败——升级仍可用，但失去开机自动恢复能力"
    pass "OTA 开机恢复服务已就位（weaknet-ota-recover.service）"

    # OTA 启动门禁（P0-C1）：独立 oneshot 单元，weaknet-server 用
    # Requires+After 依赖它。门禁失败 → 业务服务根本不被启动，且**没有
    # 重启循环**（实测：Requires 依赖失败 → result=dependency，NRestarts=0）。
    #
    # 顺序要求：门禁单元必须先落盘并 enable，再改 weaknet-server 的依赖
    # ——否则新装的 weaknet-server 会因为找不到 gate 单元而起不来。
    scp "${ROOT}/tools/weaknet-ota-gate" "${BOARD}:/tmp/weaknet-ota-gate" 2>/dev/null \
        || fail "OTA 启动门禁上传失败"
    scp "${ROOT}/tools/weaknet-ota-gate.service" "${BOARD}:/tmp/weaknet-ota-gate.service" 2>/dev/null \
        || fail "OTA 启动门禁单元上传失败"
    ssh "${BOARD}" "sudo install -m 0755 /tmp/weaknet-ota-gate \
        /usr/local/lib/weaknet/weaknet-ota-gate && \
        sudo cp /tmp/weaknet-ota-gate.service /etc/systemd/system/weaknet-ota-gate.service && \
        sudo systemctl daemon-reload && \
        sudo systemctl enable weaknet-ota-gate.service" \
        || fail "OTA 启动门禁安装失败"
    pass "OTA 启动门禁已就位（weaknet-ota-gate.service）"

    # 业务服务单元必须在门禁单元之后落盘：新 unit 的 [Unit] 段写着
    # `Requires=weaknet-ota-gate.service`，先装它会得到一个指向不存在单元的
    # 依赖，daemon-reload 后 weaknet-server 直接起不来。
    ssh "${BOARD}" "sudo cp /tmp/weaknet-server.service /etc/systemd/system/weaknet-server.service && \
        sudo systemctl daemon-reload && \
        sudo systemctl enable weaknet-server"
    pass "weaknet-server 单元已落盘（依赖 weaknet-ota-gate.service）"

    # OTA 布局迁移/切换（板端重启唯一入口）：暂存产物 → releases/${VERSION}
    # →（首次）旧扁平布局归档 legacy-flat → 原子翻转 current + 兼容软链
    # → 重启 → 健康窗口（失败自动回滚上一版）→ .committed 转正。
    #
    # adopt 之前先做一次 bootstrap：老设备没有事务状态文件，控制器需要
    # 从现有 current + .committed 推断稳定版本，建立 schema v1 记录。
    # bootstrap 拒绝覆盖已存在的状态，所以只在首次执行（状态文件不存在时）。
    # 失败不阻断部署：adopt 的 begin 会在空状态上开新事务，但此时
    # previous_stable_version 未知，回滚退路会退化——所以要显式告警。
    if ! ssh "${BOARD}" "sudo test -f /var/lib/weaknet-ota/transaction.json" 2>/dev/null; then
        if ! ssh "${BOARD}" "sudo python3 /usr/local/lib/weaknet/weaknet-ota-ctl \
            --root /home/radxa/weaknet --state-dir /var/lib/weaknet-ota bootstrap"; then
            warn "OTA 事务 bootstrap 失败（设备可能缺 .committed 标记）——升级仍可进行，但回滚退路不完整"
        else
            pass "OTA 事务状态已建立（bootstrap 接管现有布局）"
        fi
    fi
    scp "${ROOT}/tools/weaknet-local-update.sh" "${BOARD}:/tmp/weaknet-local-update.sh" 2>/dev/null

    # ===== 停掉常驻恢复服务，避免与 adopt 抢同一个 OTA 事务 =====
    #
    # 缺陷（2026-11-01 完整 ci.sh 实跑暴露）：weaknet-ota-recover 是常驻轮询
    # （30s 一次），看到 TRIAL + 服务健康就**自动 commit**——这是 P0-B 的正确
    # 设计（开机自愈），但它与 adopt 的「翻转→健康窗口→commit」是同一件事。
    # 两者抢同一个事务时恢复服务先赢了，adopt 的 commit 撞上
    # 「当前阶段 COMMITTED 不允许该操作（要求：TRIAL）」→ rc=3，
    # 被 ci.sh 判为部署失败并**跳过后续全部步骤**（隧道收尾重启、板端测试），
    # 而实际上部署已经成功。板端 journal 实锤：
    #   13:45:41 recover 判定 action=commit → 已转正（COMMITTED）
    #   13:45:41 ci.sh  健康窗口通过 → commit 被拒 → 报 adopt 失败
    #
    # 修法：部署是**人工主动操作**，期间由 ci.sh 独占事务；恢复服务的职责是
    # 「开机自愈」，不该在人工部署时抢跑。停掉它，部署结束再拉起。
    #
    # 必须配 trap：fail() 是 exit 1，中途失败会直接终止脚本，若不兜底，
    # 恢复服务将**永久停着**——下次真出事（升级后起不来）就没人自愈了。
    OTA_RECOVER_WAS_ACTIVE=false
    if ssh "${BOARD}" "systemctl is-active --quiet weaknet-ota-recover.service" 2>/dev/null; then
        OTA_RECOVER_WAS_ACTIVE=true
        ssh "${BOARD}" "sudo systemctl stop weaknet-ota-recover.service" 2>/dev/null \
            || warn "停止 weaknet-ota-recover 失败——可能与 adopt 争抢事务"
        pass "恢复服务已暂停（部署期间由 ci.sh 独占 OTA 事务）"
    fi

    # 无论成功、失败还是 Ctrl-C，都把恢复服务拉回来。
    restore_ota_recover() {
        if [ "${OTA_RECOVER_WAS_ACTIVE}" = "true" ]; then
            ssh "${BOARD}" "sudo systemctl start weaknet-ota-recover.service" 2>/dev/null \
                || echo -e "${YELLOW}⚠️  恢复服务未能自动拉起，请手工：ssh board 'sudo systemctl start weaknet-ota-recover'${NC}"
        fi
    }
    trap 'restore_ota_recover' EXIT INT TERM

    ssh "${BOARD}" "sudo install -m 0755 /tmp/weaknet-local-update.sh /home/radxa/weaknet/weaknet-local-update.sh && \
        sudo WEAKNET_ROOT=/home/radxa/weaknet /home/radxa/weaknet/weaknet-local-update.sh adopt /home/radxa/weaknet/.deploy-staging ${VERSION}" \
        || {
            # adopt 返回非零，但**不等于部署失败**：事务可能已被别的东西推进。
            # 典型是恢复服务抢跑（本次已用 trap 停掉，但仍有复活窗口），
            # 或有人手工 commit 过。判据只看客观事实——权威阶段已终结 +
            # release 已转正（.committed 存在）+ 服务在跑：
            #   * 三条都满足 → 部署其实成功了，降级为告警并**继续后续步骤**
            #     （否则会跳过隧道收尾重启与板端测试，制造"假失败"）
            #   * 否则 → 真失败，保持原来的 fail
            ota_phase=$(ssh "${BOARD}" "sudo python3 /usr/local/lib/weaknet/weaknet-ota-ctl \
                --root /home/radxa/weaknet --state-dir /var/lib/weaknet-ota status 2>/dev/null" \
                | sed -n 's/.*"phase": *"\([^"]*\)".*/\1/p' | head -1)
            has_committed=$(ssh "${BOARD}" "sudo test -f /home/radxa/weaknet/releases/${VERSION}/.committed \
                && echo yes || echo no")
            svc_state=$(ssh "${BOARD}" "systemctl is-active weaknet-server 2>/dev/null || echo unknown")
            if [ "${ota_phase}" = "COMMITTED" ] && [ "${has_committed}" = "yes" ] \
               && [ "${svc_state}" = "active" ]; then
                warn "adopt 返回非零，但事务已是 COMMITTED、releases/${VERSION} 已转正、服务正常——判定为成功"
                warn "（多为恢复服务抢跑同一事务；本次已加暂停机制，若复现请查 journalctl -u weaknet-ota-recover）"
            else
                fail "adopt 失败（phase=${ota_phase:-未知} committed=${has_committed} service=${svc_state}）——依据上方输出排查，必要时 sudo weaknet-cli update rollback"
            fi
        }
    pass "部署完成（releases/${VERSION} 已转正）"

    # 部署已完成，立即恢复常驻服务（不必等脚本整体结束——后面还有隧道重启
    # 与板端测试，那些阶段同样需要恢复服务在位）。
    restore_ota_recover
    trap - EXIT INT TERM
    pass "恢复服务已恢复运行"

    # 反向隧道收尾重启（I5，full-code-review 2026-11-01）。
    #
    # 上面落盘隧道单元时只 `enable`，注释写的"隧道重启交给 deploy 收尾"
    # 却从来没有实现——全仓 `systemctl restart weaknet-tunnel` 零处。
    # 后果：首次装机的板子要等下次重启才有反向隧道（云端 2222 不可达），
    # 改过 unit 之后改动也不生效。在"SSH 靠隧道"的拓扑下这是运维级隐患。
    #
    # 位置必须在这里（adopt 之后）：隧道重启会掐断 SSH 反向通道，
    # 而 adopt → 健康窗口 → 转正全程都走同一条通道。放在收尾处，
    # 让所有"不能被掐断"的步骤先跑完。
    #
    # **重启后必须等通道真的回来**（2026-11-01 完整 ci.sh 实跑暴露）：
    # 单元是 `Restart=always` + `RestartSec=3` + `-R 2222:localhost:22`，
    # 重启后云端 sshd 要重新绑定 2222，实测约 **50 秒**才可用。原先重启完
    # 立刻往下走，紧接着的 scp 与板端测试全部撞在断链窗口里：
    #   ssh: connect to host 127.0.0.1 port 2222: Connection refused
    # 表现为"上传失败 / chmod 失败 / 测试失败（退出码 255）"，而板端其实完好
    # ——纯属自己把通道掐了还立刻要用。这里改成轮询等待，超时才告警。
    if [ -f "${ROOT}/tools/weaknet-tunnel.service" ]; then
        # 这条 ssh 会**掐断它自己**：命令经反向隧道下发，重启隧道 = 断掉
        # 承载本次连接的通道，于是必然看到
        #   Connection to 127.0.0.1 closed by remote host.
        # ssh 因此返回 255。这是设计的必然结果（自指），不是失败信号。
        # 所以**不用返回码判定成功**，改用「重启后通道能否恢复」作准据
        # ——下方轮询才是真正的判据。这里只记录，不告警。
        ssh "${BOARD}" "sudo systemctl restart weaknet-tunnel.service" \
            && pass "反向隧道重启命令已下发" \
            || info "反向隧道已断开（预期：重启隧道必然掐断本次连接），等待其自行重建"

        # 轮询等通道恢复。总预算 90s（实测 50s 上下），每 5s 探一次。
        # 判据用一次**真正走完整 SSH 握手**的探测，不是只看端口 listen
        # ——端口早就由云端 sshd 监听着，但转发链路要等隧道会话建立后才通，
        # 只看端口会得到"假可用"。
        tunnel_wait=0
        tunnel_ok=false
        while [ "$tunnel_wait" -lt 90 ]; do
            if ssh -o ConnectTimeout=5 "${BOARD}" "true" >/dev/null 2>&1; then
                tunnel_ok=true
                break
            fi
            sleep 5
            tunnel_wait=$((tunnel_wait + 5))
        done
        if [ "$tunnel_ok" = true ]; then
            pass "反向通道已恢复（等待 ${tunnel_wait}s）"
        else
            warn "反向通道 90s 内未恢复——后续板端步骤可能全部失败"
            warn "手工检查：ssh board 是否可达；板端 systemctl status weaknet-tunnel"
        fi
    fi

    # 测试脚本上传失败不能让 set -e 在部署收尾处静默杀死整个脚本
    # （真机踩过：板端残留 root 属主旧文件 → scp EACCES → 后续步骤全没跑）
    scp "${ROOT}/tools/weaknet-test-full.sh" "${BOARD}:/home/radxa/weaknet/weaknet-test-full.sh" 2>/dev/null \
        || warn "weaknet-test-full.sh 上传失败（板端文件可能属主异常：rm 后重试）"
    ssh "${BOARD}" "chmod +x /home/radxa/weaknet/weaknet-test-full.sh" 2>/dev/null \
        || warn "weaknet-test-full.sh chmod 失败"
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

    # 上传测试脚本（失败不致命：下面的运行会因文件缺失显式报错）
    scp "${ROOT}/tools/weaknet-test-full.sh" "${BOARD}:/home/radxa/weaknet/weaknet-test-full.sh" 2>/dev/null \
        || warn "weaknet-test-full.sh 上传失败（板端文件可能属主异常：rm 后重试）"
    ssh "${BOARD}" "chmod +x /home/radxa/weaknet/weaknet-test-full.sh" 2>/dev/null \
        || warn "weaknet-test-full.sh chmod 失败"

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
