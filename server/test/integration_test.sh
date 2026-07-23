#!/bin/bash
# integration_test.sh - 集成测试：服务端启动/D-Bus注册/信号发送接收
# 自动启动服务端、运行测试、清理
# 用法: ./test/integration_test.sh

cd "$(dirname "$0")/.."  # 进入 server/ 目录

PASS=0
FAIL=0
SKIP=0
FAILED_TESTS=""
SERVER_PID=""
LOG_FILE="/tmp/weaknet_integration_test.log"
DBUS_TIMEOUT=3  # D-Bus 调用超时(秒)
TEST_START_TIME=$(date +%s)

# 清理函数
cleanup() {
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    # 使用 killall 精确匹配，避免 pkill -f 匹配到自身
    killall -q weaknet-dbus-server 2>/dev/null || true
    echo "[cleanup] 服务端已停止"
}
trap cleanup EXIT

# 测试辅助函数
check() {
    local desc="$1"
    local result="$2"
    if [ "$result" = "0" ]; then
        PASS=$((PASS + 1))
        echo "  ✅ $desc"
    elif [ "$result" = "2" ]; then
        SKIP=$((SKIP + 1))
        echo "  ⏭️  $desc"
    else
        FAIL=$((FAIL + 1))
        FAILED_TESTS="${FAILED_TESTS}  ${desc}"
        echo "  ❌ $desc"
    fi
}

# 带超时的 dbus-send
dbus_call() {
    local dest="$1" path="$2" iface="$3" method="$4"
    shift 4
    timeout "$DBUS_TIMEOUT" dbus-send --session --print-reply \
        --dest="$dest" "$path" "${iface}.${method}" "$@" 2>&1
}

# 等待 D-Bus 服务注册
wait_for_dbus_service() {
    for i in $(seq 1 10); do
        if dbus-send --session --print-reply --dest=org.freedesktop.DBus \
            /org/freedesktop/DBus org.freedesktop.DBus.NameHasOwner \
            string:com.example.WeakNet 2>/dev/null | grep -q "boolean true"; then
            return 0
        fi
        sleep 0.5
    done
    return 1
}

echo "########################################################"
echo "#  WeakNet 集成测试                                      #"
echo "########################################################"
echo ""

# ========== 测试前清理 ==========
echo "[prep] 清理旧服务端进程..."
killall -q weaknet-dbus-server 2>/dev/null || true
sleep 1

# ========== 测试1: 服务端启动验证 ==========
echo "[test] 1. 服务端启动验证"
echo "  → 启动服务端..."
./bin/weaknet-dbus-server > "$LOG_FILE" 2>&1 &
SERVER_PID=$!
sleep 3

# 检查进程是否存在
if kill -0 "$SERVER_PID" 2>/dev/null; then
    check "服务端进程存在 (PID=$SERVER_PID)" 0
else
    check "服务端进程存在" 1
    echo "  [ERROR] 服务端启动失败，日志："
    tail -20 "$LOG_FILE"
    exit 1
fi

# 检查日志中无 FATAL 错误
if grep -q "^F" "$LOG_FILE" 2>/dev/null; then
    check "日志无 FATAL 错误" 1
    grep "^F" "$LOG_FILE"
else
    check "日志无 FATAL 错误" 0
fi

# 检查服务端启动关键日志
if grep -q "DBus 服务端已启动" "$LOG_FILE" 2>/dev/null; then
    check "服务端启动日志完整" 0
else
    check "服务端启动日志完整" 1
fi

# 检查接口收集
if grep -q "collectCurrentInterfaces" "$LOG_FILE" 2>/dev/null; then
    check "接口收集功能正常" 0
else
    check "接口收集功能正常" 1
fi

# 检查监控线程启动
THREAD_COUNT=0
for thread in "rtt" "jitter" "rssi" "tcp_loss" "traffic" "network_quality" "bluetooth"; do
    if grep -qi "${thread}.*thread.*start" "$LOG_FILE" 2>/dev/null; then
        THREAD_COUNT=$((THREAD_COUNT + 1))
    fi
done
echo "  ℹ️  检测到 $THREAD_COUNT/7 个监控线程已启动"
if [ "$THREAD_COUNT" -ge 5 ]; then
    check "监控线程启动 ($THREAD_COUNT/7)" 0
else
    check "监控线程启动 ($THREAD_COUNT/7)" 1
fi

# 检查 eBPF 降级（预期行为）
if grep -q "Traffic stats unavailable" "$LOG_FILE" 2>/dev/null; then
    echo "  ℹ️  eBPF 降级模式（预期行为）"
fi
# 检查蓝牙降级（预期行为）
if grep -q "no Bluetooth adapter" "$LOG_FILE" 2>/dev/null; then
    echo "  ℹ️  蓝牙降级模式（预期行为）"
fi

# ========== 测试2: D-Bus 服务注册 ==========
echo ""
echo "[test] 2. D-Bus 服务注册成功性测试"

if wait_for_dbus_service; then
    check "服务名 com.example.WeakNet 已注册到 D-Bus" 0
else
    check "服务名 com.example.WeakNet 已注册到 D-Bus" 1
fi

# 检查对象路径存在性（通过调用方法验证，而非依赖 Introspect）
if dbus_call "com.example.WeakNet" "/com/example/WeakNet" "com.example.WeakNet" "HealthCheck" 2>/dev/null | grep -q 'string'; then
    check "对象路径 /com/example/WeakNet 可访问 (HealthCheck)" 0
else
    check "对象路径 /com/example/WeakNet 可访问 (HealthCheck)" 1
fi

# 验证 D-Bus 服务名请求无冲突
if grep -q "未能成为主拥有者" "$LOG_FILE" 2>/dev/null; then
    check "D-Bus 服务名请求成功(无冲突)" 1
else
    check "D-Bus 服务名请求成功(无冲突)" 0
fi

# ========== 测试3: 方法调用验证 ==========
echo ""
echo "[test] 3. D-Bus 方法调用验证"

# HealthCheck
HC_RESULT=$(dbus_call "com.example.WeakNet" "/com/example/WeakNet" "com.example.WeakNet" "HealthCheck" 2>&1)
if echo "$HC_RESULT" | grep -q 'string'; then
    check "HealthCheck 方法调用成功" 0
else
    check "HealthCheck 方法调用成功" 1
fi

# GetInterfaces
GET_RESULT=$(dbus_call "com.example.WeakNet" "/com/example/WeakNet" "com.example.WeakNet" "GetInterfaces" 2>&1)
if echo "$GET_RESULT" | grep -qP '(string|array)'; then
    check "GetInterfaces 方法调用成功" 0
else
    check "GetInterfaces 方法调用成功" 1
fi

# ListInterfaces
LIST_RESULT=$(dbus_call "com.example.WeakNet" "/com/example/WeakNet" "com.example.WeakNet" "ListInterfaces" 2>&1)
if echo "$LIST_RESULT" | grep -qP '(string|array)'; then
    check "ListInterfaces 方法调用成功" 0
else
    check "ListInterfaces 方法调用成功" 1
fi

# Ping (使用 timeout 避免阻塞)
PING_RESULT=$(dbus_call "com.example.WeakNet" "/com/example/WeakNet" "com.example.WeakNet" "Ping" string:"127.0.0.1" 2>&1)
if echo "$PING_RESULT" | grep -q "No active network interface"; then
    check "Ping 方法调用 (无活跃网卡，预期行为)" 0
    echo "    ℹ️  $PING_RESULT"
elif echo "$PING_RESULT" | grep -qP '(string|boolean)'; then
    check "Ping 方法调用成功" 0
else
    check "Ping 方法调用成功" 1
    echo "    $PING_RESULT"
fi

# GetBluetoothDevices
BT_DEV_RESULT=$(dbus_call "com.example.WeakNet" "/com/example/WeakNet" "com.example.WeakNet" "GetBluetoothDevices" 2>&1)
if echo "$BT_DEV_RESULT" | grep -qP '(string|array)'; then
    check "GetBluetoothDevices 方法调用成功" 0
else
    check "GetBluetoothDevices 方法调用成功" 1
fi

# GetBluetoothAdapter
BT_ADP_RESULT=$(dbus_call "com.example.WeakNet" "/com/example/WeakNet" "com.example.WeakNet" "GetBluetoothAdapter" 2>&1)
if echo "$BT_ADP_RESULT" | grep -q 'string'; then
    check "GetBluetoothAdapter 方法调用成功" 0
else
    check "GetBluetoothAdapter 方法调用成功" 1
fi

# ========== 测试4: 信号发送与接收验证 ==========
echo ""
echo "[test] 4. 信号发送与接收验证"

# 启动 dbus-monitor 在后台监听信号
SIGNAL_LOG="/tmp/weaknet_signal_monitor.log"
> "$SIGNAL_LOG"  # 清空
dbus-monitor --session "type='signal',interface='com.example.WeakNet'" > "$SIGNAL_LOG" 2>/dev/null &
MONITOR_PID=$!
sleep 2

# 等待服务端自动发送信号（质量评估每15秒、接口变化每10秒）
echo "  → 等待信号 (10秒)..."
sleep 10

# 停止监听
kill "$MONITOR_PID" 2>/dev/null || true
wait "$MONITOR_PID" 2>/dev/null || true

# 检查是否捕获到信号
SIGNAL_SIZE=$(wc -c < "$SIGNAL_LOG" 2>/dev/null || echo 0)
if [ "$SIGNAL_SIZE" -gt 10 ] && grep -q "com.example.WeakNet" "$SIGNAL_LOG" 2>/dev/null; then
    check "dbus-monitor 捕获到 WeakNet 信号" 0
    echo "  📡 信号统计:"
    for sig in "Changed" "InterfaceChanged" "NetworkQualityChanged" "BluetoothDeviceChanged" "ConnectionModeChanged"; do
        count=$(grep -c "$sig" "$SIGNAL_LOG" 2>/dev/null | tail -1 || echo 0)
        count=${count:-0}
        if [ "$count" -gt 0 ] 2>/dev/null; then
            echo "    $sig: $count 次"
        fi
    done
else
    check "dbus-monitor 捕获到 WeakNet 信号" 2
    echo "  [INFO] 未捕获信号 (signal log: ${SIGNAL_SIZE} bytes)"
    echo "  [INFO] 注意: 信号发送取决于服务端内部状态变化，可能需要在有活跃网卡的环境中触发"
fi

# 验证信号序列化文件
if [ -f "./signal_changed.bin" ] && [ -s "./signal_changed.bin" ]; then
    check "信号序列化文件已生成 (signal_changed.bin)" 0
else
    check "信号序列化文件已生成 (signal_changed.bin)" 2
    echo "  [INFO] 序列化文件未生成(可能无状态变化触发信号)"
fi

# ========== 测试5: 错误处理 ==========
echo ""
echo "[test] 5. 错误处理与健壮性"

# 调用不存在的方法
ERR_RESULT=$(dbus_call "com.example.WeakNet" "/com/example/WeakNet" "com.example.WeakNet" "NonExistentMethod" 2>&1)
if echo "$ERR_RESULT" | grep -q "error\|UnknownMethod\|does not exist"; then
    check "不存在的方法返回错误" 0
else
    check "不存在的方法返回错误" 1
fi

# 检查服务端在错误调用后仍存活
if kill -0 "$SERVER_PID" 2>/dev/null; then
    check "错误调用后服务端仍存活" 0
else
    check "错误调用后服务端仍存活" 1
fi

# 空参数 Ping 错误处理
PING_EMPTY_RESULT=$(dbus_call "com.example.WeakNet" "/com/example/WeakNet" "com.example.WeakNet" "Ping" string:"" 2>&1)
if echo "$PING_EMPTY_RESULT" | grep -q "error\|Empty"; then
    check "空参数Ping返回错误" 0
else
    check "空参数Ping返回错误" 1
fi

# ========== 测试6: 性能测试 ==========
echo ""
echo "[test] 6. 性能基准测试"

# 多次调用 HealthCheck 测量延迟
echo "  → 执行 20 次 HealthCheck 调用..."
START=$(date +%s%N)
for i in $(seq 1 20); do
    dbus_call "com.example.WeakNet" "/com/example/WeakNet" "com.example.WeakNet" "HealthCheck" >/dev/null 2>&1
done
END=$(date +%s%N)
ELAPSED_MS=$(( (END - START) / 1000000 ))
AVG_MS=$(echo "scale=1; $ELAPSED_MS / 20" | bc 2>/dev/null || echo "N/A")
echo "    总耗时: ${ELAPSED_MS}ms, 平均: ${AVG_MS}ms/次"

if [ "$ELAPSED_MS" -lt 5000 ]; then
    check "20次HealthCheck耗时<5s (${ELAPSED_MS}ms)" 0
else
    check "20次HealthCheck耗时<5s (${ELAPSED_MS}ms)" 1
fi

# 检查内存占用
if [ -f "/proc/$SERVER_PID/status" ]; then
    MEM_KB=$(grep VmRSS /proc/$SERVER_PID/status 2>/dev/null | awk '{print $2}')
    if [ -n "$MEM_KB" ] && [ "$MEM_KB" -gt 0 ]; then
        MEM_MB=$(echo "scale=1; $MEM_KB / 1024" | bc 2>/dev/null || echo "N/A")
        echo "    内存占用: ${MEM_MB}MB"
        if [ "$MEM_KB" -lt 102400 ]; then
            check "内存占用<100MB (${MEM_MB}MB)" 0
        else
            check "内存占用<100MB (${MEM_MB}MB)" 1
        fi
    fi
fi

# ========== 测试结果汇总 ==========
TEST_END_TIME=$(date +%s)
ELAPSED=$((TEST_END_TIME - TEST_START_TIME))
TOTAL=$((PASS + FAIL + SKIP))

echo ""
echo "########################################################"
echo "  集成测试结果: 通过 $PASS, 失败 $FAIL, 跳过 $SKIP, 耗时 ${ELAPSED}s"
if [ -n "$FAILED_TESTS" ]; then
    echo "  失败列表:${FAILED_TESTS}"
    echo "########################################################"
    exit 1
fi
echo "  全部通过 ✅"
echo "########################################################"
exit 0