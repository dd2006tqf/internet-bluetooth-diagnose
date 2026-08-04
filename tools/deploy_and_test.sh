#!/usr/bin/env bash
# ============================================================================
# WeakNet 一键编译 → scp 部署 → 远程全面测试脚本
#
# 功能:
#   1. 使用常驻 ARM64 容器交叉编译所有产物（服务端 + 6 个 .bpf.o + 客户端库
#      + test-client + 单元测试二进制 + test_ebpf）
#   2. 在开发板上先删除旧产物，然后 scp 部署最新产物
#   3. 远程执行全面测试脚本（功能测试 + 集成测试 + eBPF 测试）
#
# 用法:
#   ./tools/deploy_and_test.sh
#
# 环境变量:
#   CONTAINER - ARM64 构建容器名（默认: weaknet-arm64-dev）
#   BOARD     - 开发板 SSH 地址（默认: radxa@192.168.2.77）
#   BOARD_DIR - 开发板部署目录（默认: /home/radxa/weaknet）
#   JOBS      - 编译并行度（默认: 2）
# ============================================================================
set -euo pipefail

# ---- 配置 ----
CONTAINER="${CONTAINER:-weaknet-arm64-dev}"
BOARD="${BOARD:-radxa@192.168.2.77}"
BOARD_DIR="${BOARD_DIR:-/home/radxa/weaknet}"
JOBS="${JOBS:-2}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIST_DIR="${ROOT}/dist-arm64"

# ---- 颜色 ----
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# ============================================================================
# Step 0: 前置检查
# ============================================================================
echo "=============================================="
echo "  WeakNet 一键编译 → 部署 → 测试"
echo "=============================================="
echo "容器:   ${CONTAINER}"
echo "开发板: ${BOARD}"
echo "目标:   ${BOARD_DIR}"
echo "并行度: ${JOBS}"
echo ""

# ---- 0.1 检查容器 ----
echo "===== Step 0: 前置检查 ====="
if ! docker inspect "${CONTAINER}" >/dev/null 2>&1; then
    echo "错误: 容器 ${CONTAINER} 不存在"
    echo "请先按 docs/交叉编译与开发板部署.md 创建常驻 ARM64 构建容器"
    exit 1
fi
if [ "$(docker inspect -f '{{.State.Running}}' "${CONTAINER}")" != "true" ]; then
    echo "启动容器 ${CONTAINER} ..."
    docker start "${CONTAINER}" >/dev/null
fi
echo "容器 ${CONTAINER} 已就绪（Up）"
echo ""

# ============================================================================
# Step 1: 在容器内完整交叉编译
# ============================================================================
echo "===== Step 1: ARM64 交叉编译（并行度 -j${JOBS}）====="

docker exec -e JOBS="${JOBS}" "${CONTAINER}" bash -lc '
set -euo pipefail
cd /src

# 复制开发板 vmlinux.h（仅供 eBPF 编译使用，非交叉编译）
mkdir -p server/build
cp board-assets/vmlinux.h server/build/vmlinux.h 2>/dev/null || true

echo "--- 编译服务端 + 全部 6 个 BPF 程序 ---"
make -C server -j"$JOBS"

echo "--- 编译客户端库 + test-client ---"
make client-lib -j"$JOBS"

echo "--- 清理旧测试产物（防止宿主 x86 残留被 make 跳过）---"
rm -rf server/test/bin

echo "--- 编译单元测试二进制 ---"
make -C server test-build -j"$JOBS"

echo "--- 编译 eBPF 测试程序（只编译不运行，避免 sudo）---"
make -C server test/test_ebpf -j"$JOBS"
'

# ---- 架构验证 ----
echo ""
echo "===== 架构验证 ====="
ARCH_OK=true
for f in \
    server/bin/weaknet-dbus-server \
    client/bin/test-client \
    client/lib/libweaknet.so \
    server/test/test_ebpf
do
    if [ -f "$f" ]; then
        arch_info=$(file "$f")
        echo "  $arch_info"
        if ! echo "$arch_info" | grep -q "ARM aarch64"; then
            echo "  警告: $f 不是 ARM64 架构！"
            ARCH_OK=false
        fi
    else
        echo "  文件不存在: $f（跳过）"
    fi
done

if [ "$ARCH_OK" = false ]; then
    echo "错误: 架构验证失败，请检查交叉编译环境"
    exit 1
fi
echo ""

# ============================================================================
# Step 2: 打包部署目录
# ============================================================================
echo "===== Step 2: 打包部署目录 ====="

rm -rf "${DIST_DIR}"

# 创建目录结构
mkdir -p \
    "${DIST_DIR}/server/bin" \
    "${DIST_DIR}/server/build" \
    "${DIST_DIR}/server/logs/server" \
    "${DIST_DIR}/server/test/bin" \
    "${DIST_DIR}/client/bin" \
    "${DIST_DIR}/client/lib" \
    "${DIST_DIR}/lib"

# 复制产物
echo "复制服务端二进制..."
install -m 0755 server/bin/weaknet-dbus-server "${DIST_DIR}/server/bin/weaknet-dbus-server"

echo "复制全部 6 个 eBPF 字节码..."
for bpf in flow_rate a2dp_media tcp_retransmit dns_monitor wifi_packet_loss http_latency; do
    src="server/build/${bpf}.bpf.o"
    if [ -f "$src" ]; then
        install -m 0644 "$src" "${DIST_DIR}/server/build/${bpf}.bpf.o"
    else
        echo "  警告: $src 不存在，跳过"
    fi
done

echo "复制客户端库和测试程序..."
install -m 0755 client/bin/test-client "${DIST_DIR}/client/bin/test-client"
install -m 0644 client/lib/libweaknet.so "${DIST_DIR}/client/lib/libweaknet.so"

echo "复制 libbpf 运行时依赖..."
if [ -d lib ] && ls lib/libbpf.so* >/dev/null 2>&1; then
    cp -a lib/libbpf.so* "${DIST_DIR}/lib/"
elif docker cp "${CONTAINER}:/usr/local/lib/libbpf.so.1.3.0" "${DIST_DIR}/lib/" 2>/dev/null; then
    (cd "${DIST_DIR}/lib" && ln -sf libbpf.so.1.3.0 libbpf.so.1 && ln -sf libbpf.so.1 libbpf.so)
else
    echo "  警告: 未找到 libbpf 运行时库"
fi

echo "复制单元测试二进制..."
if [ -d server/test/bin ]; then
    cp -a server/test/bin/* "${DIST_DIR}/server/test/bin/" 2>/dev/null || true
fi
# test_bt_monitor 定义在 test/special/ 下，单独复制到 test/bin/
if [ -f server/test/special/test_bt_monitor ]; then
    install -m 0755 server/test/special/test_bt_monitor "${DIST_DIR}/server/test/bin/test_bt_monitor"
fi

echo "复制 eBPF 测试程序..."
if [ -f server/test/test_ebpf ]; then
    install -m 0755 server/test/test_ebpf "${DIST_DIR}/server/test/test_ebpf"
fi

# ---- 写入板端完整测试脚本 ----
echo "写入板端测试脚本..."
cat > "${DIST_DIR}/weaknet-test-full.sh" << 'REMOTE_EOF'
#!/usr/bin/env bash
# ============================================================================
# WeakNet 开发板全面测试脚本
# 在私有 D-Bus 会话中启动服务端，然后逐项运行所有测试
# ============================================================================

export HOME=/home/radxa
export LD_LIBRARY_PATH=/home/radxa/weaknet/lib:/home/radxa/weaknet/client/lib:/usr/local/lib

exec dbus-run-session -- bash -lc '
set -u

# ---- 配置 ----
PASS=0
FAIL=0
SKIP=0

ok()   { echo -e "  \033[0;32m[PASS]\033[0m $1"; ((PASS++)); }
fail()  { echo -e "  \033[0;31m[FAIL]\033[0m $1"; ((FAIL++)); }
skip()  { echo -e "  \033[1;33m[SKIP]\033[0m $1"; ((SKIP++)); }

ulimit -l unlimited 2>/dev/null || true

cd /home/radxa/weaknet/server
mkdir -p logs/server
: > /tmp/weaknet-server.log

echo "=============================================="
echo "  WeakNet 开发板全面测试"
echo "=============================================="
echo ""

# =============================================
# Phase 1: 启动服务端
# =============================================
echo "===== Phase 1: 启动服务端 ====="

./bin/weaknet-dbus-server >/tmp/weaknet-server.log 2>&1 &
SERVER_PID=$!

cleanup() {
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
}
trap cleanup EXIT

echo "等待服务端启动 (15s)..."
sleep 15

if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "  [FAIL] 服务端启动失败"
    echo "----- 服务端日志 -----"
    cat /tmp/weaknet-server.log
    echo "---------------------"
    exit 1
fi
echo "  [PASS] 服务端运行中 (PID=$SERVER_PID)"
PASS=$((PASS + 1))

cd /home/radxa/weaknet

# ==============================================
# Phase 2-10: 客户端测试（依赖服务端）
# ==============================================

run_client_test() {
    local name="$1"
    local cmd="$2"
    echo ""
    echo "----- $name -----"
    if ./client/bin/test-client $cmd 2>&1; then
        ok "$name"
    else
        fail "$name"
    fi
}

# Phase 2: 服务端健康检查
echo ""
echo "===== Phase 2: 服务端基础检查 ====="
run_client_test "health" "health"
run_client_test "get" "get"

# Phase 3: 基础功能测试
echo ""
echo "===== Phase 3: 基础功能测试 ====="
run_client_test "test-basic" "test-basic"
run_client_test "test-network" "test-network"

# Phase 4: Ping 测试
echo ""
echo "===== Phase 4: Ping 测试 ====="
run_client_test "test-ping" "test-ping"

# Phase 5: 事件系统测试
echo ""
echo "===== Phase 5: 事件系统测试 ====="
run_client_test "test-events" "test-events"

# Phase 6: 网络质量测试
echo ""
echo "===== Phase 6: 网络质量测试 ====="
run_client_test "test-quality" "test-quality"

# Phase 7: 蓝牙测试
echo ""
echo "===== Phase 7: 蓝牙测试（可能 SKIP）====="
run_client_test "test-bt" "test-bt"

# Phase 8: 错误处理测试
echo ""
echo "===== Phase 8: 错误处理测试 ====="
run_client_test "test-errors" "test-errors"

# Phase 9: 性能测试
echo ""
echo "===== Phase 9: 性能测试 ====="
run_client_test "test-performance" "test-performance"

# Phase 10: 单项命令验证
echo ""
echo "===== Phase 10: 单项命令验证 ====="
run_client_test "check" "check"
run_client_test "event-types" "event-types"

# ==============================================
# Phase 11: 单元测试（不依赖服务端）
# ==============================================
echo ""
echo "===== Phase 11: 单元测试 ====="
cd /home/radxa/weaknet/server
for utest in test/bin/*; do
    if [ -x "$utest" ]; then
        echo "  --- 运行 $utest ---"
        if "./$utest" 2>&1; then
            ok "单元测试: $(basename $utest)"
        else
            fail "单元测试: $(basename $utest)"
        fi
    fi
done
cd /home/radxa/weaknet

# ==============================================
# Phase 12: eBPF 测试
# ==============================================
echo ""
echo "===== Phase 12: eBPF 测试 ====="
if [ -x server/test/test_ebpf ]; then
    if sudo -n true 2>/dev/null; then
        echo "  --- 运行 eBPF 测试（sudo）---"
        # 生成一些背景流量让 eBPF 有数据可采
        curl -s -o /dev/null https://www.baidu.com 2>/dev/null || true
        if sudo ./server/test/test_ebpf 2>&1; then
            ok "eBPF 测试"
        else
            fail "eBPF 测试"
        fi
    else
        skip "eBPF 测试（需要 sudo -n 免密码权限）"
    fi
else
    skip "eBPF 测试程序不存在"
fi

# Phase 13: eBPF 挂载检查
echo ""
echo "===== Phase 13: eBPF 挂载检查 ====="
if command -v bpftool &>/dev/null || [ -x /usr/sbin/bpftool ]; then
    BPFTOOL=$(command -v bpftool || echo /usr/sbin/bpftool)
    bpf_total=$(sudo "$BPFTOOL" prog show 2>/dev/null | grep -c "^[0-9]*:" || true)
    if [ "$bpf_total" -gt 0 ]; then
        ok "eBPF 程序存在（$bpf_total 个）"
    else
        skip "eBPF 程序挂载检查（未检测到）"
    fi
else
    skip "bpftool 不可用，跳过 eBPF 挂载检查"
fi

# Phase 14: 服务端日志
echo ""
echo "===== Phase 14: 服务端日志（最后 80 行）====="
tail -n 80 /tmp/weaknet-server.log

# ==============================================
# 汇总
# ==============================================
echo ""
echo "=============================================="
echo "  测试汇总"
echo "=============================================="
echo "  PASS: $PASS"
echo "  FAIL: $FAIL"
echo "  SKIP: $SKIP"
echo "=============================================="
echo ""

exit $FAIL
'
REMOTE_EOF

chmod +x "${DIST_DIR}/weaknet-test-full.sh"

echo "打包完成，产物列表："
find "${DIST_DIR}" -type f | sort
echo ""

# ============================================================================
# Step 3: scp 部署到开发板
# ============================================================================
echo "===== Step 3: scp 部署到开发板 ====="

# 检查连通性
if ! ping -c 1 -W 2 "${BOARD#*@}" >/dev/null 2>&1; then
    echo "错误: 开发板 ${BOARD} 不可达（ping 超时）"
    echo "部署目录已打包在 ${DIST_DIR}"
    echo "待开发板在线后，运行以下命令继续："
    echo "  ssh ${BOARD} \"rm -rf ${BOARD_DIR} && mkdir -p ${BOARD_DIR}\""
    echo "  scp -r ${DIST_DIR}/* ${BOARD}:${BOARD_DIR}/"
    echo "  ssh -t ${BOARD} \"sudo ${BOARD_DIR}/weaknet-test-full.sh\""
    exit 1
fi

echo "开发板 ${BOARD} 可达，开始部署..."

# 先删除旧产物（部分文件由 sudo 创建，需 sudo 删除）
ssh "${BOARD}" "echo radxa | sudo -S rm -rf '${BOARD_DIR}' && mkdir -p '${BOARD_DIR}'" 2>&1
echo "  [OK] 已删除旧产物"

# scp 传输
scp -r "${DIST_DIR}"/* "${BOARD}:${BOARD_DIR}/" 2>&1
echo "  [OK] scp 传输完成"
echo ""

# ============================================================================
# Step 4: 远程执行全面测试
# ============================================================================
echo "===== Step 4: 远程执行全面测试 ====="
echo ""

ssh -t "${BOARD}" "sudo '${BOARD_DIR}/weaknet-test-full.sh'"
REMOTE_EXIT=$?

echo ""
if [ $REMOTE_EXIT -eq 0 ]; then
    echo -e "${GREEN}===== 全部测试通过! =====${NC}"
else
    echo -e "${RED}===== 有 ${REMOTE_EXIT} 个测试失败 =====${NC}"
fi
echo "开发板部署目录: ${BOARD_DIR}"
echo ""

exit $REMOTE_EXIT
