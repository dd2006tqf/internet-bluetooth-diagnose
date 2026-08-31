#!/usr/bin/env bash
# ============================================================================
# WeakNet 开发板全面测试脚本
# 服务端与客户端均通过 D-Bus 系统总线通信（/etc/dbus-1/system.d/
# com.example.WeakNet.conf 已放行本地用户访问）。
# 测试前会先停掉 systemd 常驻实例，避免系统总线上服务名冲突；
# 测试结束后自动恢复 systemd 服务。
#
# 用法:
#   ssh radxa@radxa-cubie-a7a.local 'sudo /home/radxa/weaknet/weaknet-test-full.sh'
#   # 或通过 ci.sh 自动调用
# ============================================================================

export HOME=/home/radxa
export LD_LIBRARY_PATH=/home/radxa/weaknet/lib:/home/radxa/weaknet/client/lib:/usr/local/lib

set -u

# ---- 配置 ----
PASS=0
FAIL=0
SKIP=0

ok()   { echo -e "  \033[0;32m[PASS]\033[0m $1"; ((PASS++)); }
fail()  { echo -e "  \033[0;31m[FAIL]\033[0m $1"; ((FAIL++)); }
skip()  { echo -e "  \033[1;33m[SKIP]\033[0m $1"; ((SKIP++)); }

ulimit -l unlimited 2>/dev/null || true

# 系统总线上只允许一个 com.example.WeakNet：先停掉 systemd 常驻实例。
# 注意：pkill -f 的模式用 [s] 方括号 trick，避免匹配到脚本自身
# （内层 bash -lc 的命令行里包含模式文本，无 trick 会把自己杀掉导致脚本无输出退出）。
systemctl stop weaknet-server 2>/dev/null || true
pkill -f "weaknet-dbus-[s]erver" 2>/dev/null || true
sleep 1

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
    # 恢复 systemd 常驻实例（生产状态）
    systemctl restart weaknet-server 2>/dev/null || true
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
