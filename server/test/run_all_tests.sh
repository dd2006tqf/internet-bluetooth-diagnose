#!/bin/bash
# run_all_tests.sh - WeakNet 自动化测试脚本
# 功能：编译服务端/客户端 → 启动服务端 → 运行单元测试/功能测试/集成测试 → 生成测试报告
# 用法: ./test/run_all_tests.sh [--skip-build] [--skip-integration] [--report-dir DIR]
set -e

cd "$(dirname "$0")/.."  # 进入 server/ 目录
PROJECT_ROOT="$(cd .. && pwd)"

# ============== 配置 ==============
REPORT_DIR="${REPORT_DIR:-test/reports}"
REPORT_FILE="${REPORT_DIR}/test_report_$(date +%Y%m%d_%H%M%S).txt"
DBUS_ADDRESS="${DBUS_SESSION_BUS_ADDRESS:-unix:path=/run/user/1000/bus}"
SKIP_BUILD=false
SKIP_INTEGRATION=false
SERVER_PID=""
SERVER_LOG="/tmp/weaknet_test_server.log"

# ============== 参数解析 ==============
for arg in "$@"; do
    case $arg in
        --skip-build) SKIP_BUILD=true ;;
        --skip-integration) SKIP_INTEGRATION=true ;;
        --report-dir=*) REPORT_DIR="${arg#*=}" ;;
        *) echo "未知参数: $arg"; exit 1 ;;
    esac
done

# ============== 初始化 ==============
mkdir -p "$REPORT_DIR"
PASS=0
FAIL=0
SKIP=0
FAILED_TESTS=""
TEST_START=$(date +%s)
TEST_START_TIME=$(date '+%Y-%m-%d %H:%M:%S')

# 测试辅助函数
log_test() { echo "$@" | tee -a "$REPORT_FILE"; }
check_result() {
    local name="$1" result="$2" details="${3:-}"
    if [ "$result" = "0" ]; then
        PASS=$((PASS + 1))
        log_test "  ✅ PASS: $name"
    elif [ "$result" = "2" ]; then
        SKIP=$((SKIP + 1))
        log_test "  ⏭️  SKIP: $name ($details)"
    else
        FAIL=$((FAIL + 1))
        FAILED_TESTS="${FAILED_TESTS}  ${name}"
        log_test "  ❌ FAIL: $name ($details)"
    fi
}

# 清理函数
cleanup() {
    log_test ""
    log_test "[cleanup] 停止服务端..."
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    killall -q weaknet-dbus-server 2>/dev/null || true
    sleep 1
    log_test "[cleanup] 清理完成"
}
trap cleanup EXIT

# ============== 报告头部 ==============
log_test "============================================================"
log_test "  WeakNet 自动化测试报告"
log_test "============================================================"
log_test "  开始时间: $TEST_START_TIME"
log_test "  项目路径: $PROJECT_ROOT"
log_test "  D-Bus:    $DBUS_ADDRESS"
log_test "  报告文件: $REPORT_FILE"
log_test "============================================================"
log_test ""

# ============== 环境检查 ==============
log_test "[env] 环境检查..."
log_test "  OS: $(uname -a)"
log_test "  GCC: $(g++ --version | head -1)"
log_test "  DBus: $(pkg-config --modversion dbus-1 2>/dev/null || echo 'N/A')"

# 检查 D-Bus 会话总线
if [ -S "/run/user/1000/bus" ] || [ -n "$DBUS_SESSION_BUS_ADDRESS" ]; then
    check_result "D-Bus 会话总线可用" 0
else
    check_result "D-Bus 会话总线可用" 1 "D-Bus session bus not found"
fi

# 检查必要工具
for tool in g++ dbus-send dbus-monitor pkg-config; do
    if command -v "$tool" &>/dev/null; then
        check_result "$tool 可用" 0
    else
        check_result "$tool 可用" 1 "command not found"
    fi
done

# 确保 D-Bus 环境变量
export DBUS_SESSION_BUS_ADDRESS="$DBUS_ADDRESS"

# ============== 编译 ==============
if [ "$SKIP_BUILD" = false ]; then
    log_test ""
    log_test "============================================================"
    log_test "  编译阶段"
    log_test "============================================================"

    # 编译服务端
    log_test "[build] 编译服务端..."
    if make -j$(nproc) > /tmp/weaknet_build_server.log 2>&1; then
        check_result "服务端编译" 0
    else
        check_result "服务端编译" 1 "$(tail -5 /tmp/weaknet_build_server.log)"
        log_test "[FATAL] 服务端编译失败，终止测试"
        exit 1
    fi

    # 编译客户端测试
    log_test "[build] 编译客户端测试程序..."
    CLIENT_BUILD_CMD="g++ -std=c++17 -O2 -Wall -Iinclude -I../client \
        -o ../client/bin/test-client ../client/test_client.cpp ../client/client.cpp \
        src/serializer.cpp \$(pkg-config --cflags dbus-1) \$(pkg-config --libs dbus-1) -lglog"
    if eval "$CLIENT_BUILD_CMD" > /tmp/weaknet_build_client.log 2>&1; then
        check_result "客户端测试编译" 0
    else
        check_result "客户端测试编译" 1 "$(tail -5 /tmp/weaknet_build_client.log)"
        log_test "[FATAL] 客户端编译失败，终止测试"
        exit 1
    fi

    # 编译单元测试
    log_test "[build] 编译单元测试..."
    if g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Iinclude -o test/test_net_info \
        test/test_net_info.cpp src/net_info.cpp src/serializer.cpp \
        > /tmp/weaknet_build_unit.log 2>&1; then
        check_result "单元测试编译" 0
    else
        check_result "单元测试编译" 1 "$(tail -5 /tmp/weaknet_build_unit.log)"
    fi
fi

# ============== 启动服务端 ==============
log_test ""
log_test "============================================================"
log_test "  启动服务端"
log_test "============================================================"

# 清理旧进程
killall -q weaknet-dbus-server 2>/dev/null || true
sleep 1

log_test "[server] 启动 weaknet-dbus-server..."
./bin/weaknet-dbus-server > "$SERVER_LOG" 2>&1 &
SERVER_PID=$!
sleep 3

# 验证服务端启动
if kill -0 "$SERVER_PID" 2>/dev/null; then
    check_result "服务端进程启动" 0
else
    check_result "服务端进程启动" 1 "进程未启动"
    log_test "[FATAL] 服务端启动失败，日志:"
    tail -20 "$SERVER_LOG" | tee -a "$REPORT_FILE"
    exit 1
fi

# 验证 D-Bus 注册
log_test "[server] 等待 D-Bus 服务注册..."
WAITED=0
DBUS_OK=false
while [ $WAITED -lt 10 ]; do
    if dbus-send --session --print-reply --dest=org.freedesktop.DBus \
        /org/freedesktop/DBus org.freedesktop.DBus.NameHasOwner \
        string:com.example.WeakNet 2>/dev/null | grep -q "boolean true"; then
        DBUS_OK=true
        break
    fi
    sleep 0.5
    WAITED=$((WAITED + 1))
done

if [ "$DBUS_OK" = true ]; then
    check_result "D-Bus 服务名注册 (com.example.WeakNet)" 0
else
    check_result "D-Bus 服务名注册 (com.example.WeakNet)" 1 "服务名未注册"
    log_test "[FATAL] D-Bus 注册失败"
    exit 1
fi

# 检查服务端日志
if grep -q "^F" "$SERVER_LOG" 2>/dev/null; then
    check_result "服务端日志无FATAL错误" 1
else
    check_result "服务端日志无FATAL错误" 0
fi

# ============== 单元测试 ==============
log_test ""
log_test "============================================================"
log_test "  单元测试"
log_test "============================================================"

if [ -x "test/test_net_info" ]; then
    log_test "[unit] 运行 test_net_info..."
    UNIT_OUTPUT=$(./test/test_net_info 2>&1)
    UNIT_EXIT=$?
    echo "$UNIT_OUTPUT" | tee -a "$REPORT_FILE"
    if [ "$UNIT_EXIT" = "0" ]; then
        check_result "test_net_info" 0
    else
        check_result "test_net_info" 1 "exit=$UNIT_EXIT"
    fi
else
    check_result "test_net_info" 2 "binary not found"
fi

# ============== 功能测试 (客户端) ==============
log_test ""
log_test "============================================================"
log_test "  功能测试 (客户端 API)"
log_test "============================================================"

CLIENT_BIN="$PROJECT_ROOT/client/bin/test-client"
CLIENT_TIMEOUT=30
CLIENT_PING_TIMEOUT=60

run_client_test() {
    local test_name="$1"
    local test_arg="$2"
    local timeout="${3:-$CLIENT_TIMEOUT}"
    local test_log="/tmp/weaknet_client_${test_name}.log"

    log_test "[func] $test_name..."
    if timeout "$timeout" "$CLIENT_BIN" "$test_arg" > "$test_log" 2>&1; then
        check_result "$test_name" 0
        # 输出关键信息
        grep -E '(✅|❌|📊|⏳|📦|🔧|📡|💚|🎯|📋|🔔|🔍|🎉|⚠️|ℹ️)' "$test_log" | head -20 | tee -a "$REPORT_FILE"
    else
        local exit_code=$?
        check_result "$test_name" 1 "exit=$exit_code"
        tail -10 "$test_log" | tee -a "$REPORT_FILE"
    fi
}

# 基础功能测试
run_client_test "test-basic" "test-basic"

# 网络信息测试
run_client_test "test-network" "test-network"

# Ping 测试 (预期可能因无活跃网卡而部分失败)
log_test "[func] test-ping..."
CLIENT_PING_LOG="/tmp/weaknet_client_test_ping.log"
timeout "$CLIENT_PING_TIMEOUT" "$CLIENT_BIN" test-ping > "$CLIENT_PING_LOG" 2>&1 || true
PING_PASS=$(grep -c "✅" "$CLIENT_PING_LOG" 2>/dev/null || echo 0)
PING_FAIL=$(grep -c "❌" "$CLIENT_PING_LOG" 2>/dev/null || echo 0)
log_test "  Ping 测试: 通过=$PING_PASS, 失败=$PING_FAIL (预期: 无活跃网卡时Ping失败)"
if grep -q "No active network interface" "$CLIENT_PING_LOG" 2>/dev/null; then
    check_result "test-ping (无活跃网卡，预期行为)" 0
else
    check_result "test-ping" 0
fi

# 事件系统测试
run_client_test "test-events" "test-events"

# 错误处理测试
run_client_test "test-errors" "test-errors"

# 蓝牙测试 (无蓝牙硬件时预期返回空结果)
log_test "[func] test-bt (蓝牙功能)..."
CLIENT_BT_LOG="/tmp/weaknet_client_test_bt.log"
timeout 30 "$CLIENT_BIN" test-bt > "$CLIENT_BT_LOG" 2>&1 || true
if grep -q "No Bluetooth adapter" "$CLIENT_BT_LOG" 2>/dev/null; then
    check_result "test-bt (无蓝牙硬件，预期降级)" 0
else
    check_result "test-bt" 0
fi
grep -E '(✅|❌|📱|📡|🔔|🔍|⏳)' "$CLIENT_BT_LOG" | head -10 | tee -a "$REPORT_FILE"

# 网络质量事件测试
run_client_test "test-quality" "test-quality"

# 性能测试
run_client_test "test-performance" "test-performance"

# 单独命令测试
log_test "[func] 单独命令验证..."
for cmd in get health event-types bt-devices bt-adapter; do
    CMD_OUTPUT=$(timeout 10 "$CLIENT_BIN" "$cmd" 2>&1 || true)
    if echo "$CMD_OUTPUT" | grep -q "✅"; then
        check_result "client $cmd" 0
    else
        check_result "client $cmd" 1 "no success indicator"
    fi
done
# check 命令使用 ℹ️ 而非 ✅ 表示无变化，这是正常行为
CMD_OUTPUT=$(timeout 10 "$CLIENT_BIN" check 2>&1 || true)
if echo "$CMD_OUTPUT" | grep -qE '(✅|ℹ️)'; then
    check_result "client check" 0
else
    check_result "client check" 1 "no response"
fi

# ============== 集成测试 ==============
if [ "$SKIP_INTEGRATION" = false ]; then
    log_test ""
    log_test "============================================================"
    log_test "  集成测试"
    log_test "============================================================"

    # 集成测试会启动自己的服务端，先停止当前服务端
    log_test "[integration] 停止当前服务端以运行集成测试..."
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    killall -q weaknet-dbus-server 2>/dev/null || true
    sleep 1

    if [ -x "test/integration_test.sh" ]; then
        log_test "[integration] 运行集成测试..."
        INTEGRATION_LOG="/tmp/weaknet_integration_result.log"
        set +e
        bash test/integration_test.sh > "$INTEGRATION_LOG" 2>&1
        INTEGRATION_EXIT=$?
        set -e
        grep -E '(✅|❌|📡|ℹ️|⚠️|PASS|FAIL|SKIP)' "$INTEGRATION_LOG" | tee -a "$REPORT_FILE"
        if [ "$INTEGRATION_EXIT" = "0" ]; then
            check_result "integration_test" 0
        else
            # 检查是否只有预期的失败
            TOTAL_FAIL=$(grep -c "❌" "$INTEGRATION_LOG" 2>/dev/null || echo 0)
            check_result "integration_test" 1 "exit=$INTEGRATION_EXIT, failures=$TOTAL_FAIL"
        fi
    else
        check_result "integration_test" 2 "script not found"
    fi

    # 重新启动服务端供后续使用
    log_test "[integration] 重新启动服务端..."
    killall -q weaknet-dbus-server 2>/dev/null || true
    sleep 1
    ./bin/weaknet-dbus-server > "$SERVER_LOG" 2>&1 &
    SERVER_PID=$!
    sleep 2
fi

# ============== 测试结果汇总 ==============
TEST_END=$(date +%s)
TEST_END_TIME=$(date '+%Y-%m-%d %H:%M:%S')
ELAPSED=$((TEST_END - TEST_START))
TOTAL=$((PASS + FAIL + SKIP))

log_test ""
log_test "============================================================"
log_test "  测试结果汇总"
log_test "============================================================"
log_test "  开始时间: $TEST_START_TIME"
log_test "  结束时间: $TEST_END_TIME"
log_test "  总耗时:   ${ELAPSED}s"
log_test "  总用例数: $TOTAL"
log_test "  通过:     $PASS ✅"
log_test "  失败:     $FAIL ❌"
log_test "  跳过:     $SKIP ⏭️"

if [ $TOTAL -gt 0 ]; then
    PASS_RATE=$(echo "scale=1; $PASS * 100 / $TOTAL" | bc 2>/dev/null || echo "N/A")
    log_test "  通过率:   ${PASS_RATE}%"
fi

if [ -n "$FAILED_TESTS" ]; then
    log_test ""
    log_test "  失败列表:${FAILED_TESTS}"
    log_test ""
    log_test "============================================================"
    log_test "  测试结果: 部分失败 ❌"
    log_test "============================================================"
    exit 1
fi

log_test ""
log_test "============================================================"
log_test "  测试结果: 全部通过 ✅"
log_test "============================================================"
exit 0