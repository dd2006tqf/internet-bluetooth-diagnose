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
# 与 systemd 单元保持一致：日志落盘到 <root>/data/logs（持久化目录）。
# 不设置时服务端会回落到相对 WorkingDirectory 的 ./logs/server，
# 手工测试写下的日志会留在版本目录里，升级即丢。
export WEAKNET_ROOT=/home/radxa/weaknet

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
mkdir -p "$WEAKNET_ROOT/data/logs"
: > /tmp/weaknet-server.log

# 记录启动前 data/logs 里已有的 .log 文件名，Phase 1.5 用它判断
# "本次进程确实新建了文件"，而不是在数上一轮的残留。
#
# 比的是**文件名集合**而不是数量：目录里已有 kFileLogMaxFiles(5) 个文件时，
# 新进程开出的第 6 个会立即触发裁剪，数量仍然是 5 —— 用计数会误判成失败。
LOG_DIR="$WEAKNET_ROOT/data/logs"
PRE_LOG_LIST=$(ls -1 "$LOG_DIR"/*.log 2>/dev/null || true)

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

# =============================================
# Phase 1.5: 文件日志落盘断言
#
# 服务端有两条日志通道：glog 的 stderr（这里重定向到 /tmp/weaknet-server.log）
# 和项目自己的 FileLogSink（落盘到 $WEAKNET_ROOT/data/logs）。
# 过去只验证前者，于是"日志目录解析错了"这类回归（历史上 glog 用
# ./logs/server、sink 用 ./server/log，都相对 WorkingDirectory 解析，
# OTA 切版本即丢）不会被任何自动断言发现。
# =============================================
echo ""
echo "===== Phase 1.5: 文件日志落盘断言 ====="

CUR_LOG_LIST=$(ls -1 "$LOG_DIR"/*.log 2>/dev/null || true)

# 找出启动后存在、启动前不存在的文件名
NEW_NAME=""
while IFS= read -r f; do
    [ -n "$f" ] || continue
    if ! printf '%s\n' "$PRE_LOG_LIST" | grep -qxF "$f"; then
        NEW_NAME="$f"
        break
    fi
done <<< "$CUR_LOG_LIST"

if [ -z "$NEW_NAME" ]; then
    fail "文件日志：data/logs 未产生新文件"
    echo "        目录: $LOG_DIR"
    ls -la "$LOG_DIR" 2>/dev/null | head -10
else
    ok "文件日志：产生了新文件 $(basename "$NEW_NAME")"

    # 取最新一个文件做内容断言
    NEWEST=$(ls -t "$LOG_DIR"/*.log 2>/dev/null | head -1)
    if [ -z "$NEWEST" ]; then
        fail "文件日志：无法定位最新文件"
    else
        # 2.1 文件头必须含版本/构建/pid：离线取证时先要确认"这份日志是谁写的"
        if grep -q "weaknet version=" "$NEWEST" 2>/dev/null; then
            ok "文件日志：文件头含版本元信息"
        else
            fail "文件日志：文件头缺少 version= 元信息（$NEWEST）"
        fi

        # 2.2 行格式必须是 [时间戳] [级别] [模块] 正文
        if grep -qE '^\[[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{6}\] \[(INFO|WARNING|ERROR|FATAL)\] \[' "$NEWEST" 2>/dev/null; then
            ok "文件日志：行格式含时间戳/级别/模块"
        else
            fail "文件日志：行格式不符（$NEWEST）"
        fi

        # 2.3 模块标签必须与 weaknet-cli 的监控器名对齐，否则 grep 不到
        if grep -qE '\[(dns|tcp_rtt|skb_drop|wifi_loss|http_latency|tcp_conn|tcp_retrans|tcp_connect|process_profiler|traffic|ebpf)\]' "$NEWEST" 2>/dev/null; then
            ok "文件日志：模块标签与监控器名对齐"
        else
            skip "文件日志：本窗口内没有细粒度模块标签可验证"
        fi
    fi
fi

# =============================================
# Phase 1.6: 崩溃调用栈可符号化
#
# InstallFailureSignalHandler 的产出质量取决于二进制的 .symtab 是否在
# （glog 0.4.0 走 /proc/self/maps + 磁盘 ELF 解析，不依赖 dladdr/rdynamic）。
# 用一个内置的小探针真崩一次：能解析出函数名才算这条能力成立。
# =============================================
echo ""
echo "===== Phase 1.6: 崩溃调用栈可符号化 ====="

CRASH_PROBE="$WEAKNET_ROOT/server/test/test_crash_handler"
if [ -x "$CRASH_PROBE" ]; then
    # --check 模式自己 fork、自己收尸、自己判定，成功退出 0
    if "$CRASH_PROBE" --check 2>&1; then
        ok "崩溃栈：SIGSEGV 已捕获且调用栈已符号化"
    else
        fail "崩溃栈：捕获或符号化失败（二进制可能被 strip）"
    fi
else
    skip "崩溃栈验证（test_crash_handler 未部署）"
fi

# =============================================
# Phase 1.7: 日志目录回归检查
# =============================================
echo ""
echo "===== Phase 1.7: 日志目录回归检查 ====="

# 日志不得再落进版本目录（相对 WorkingDirectory 的老路径）
#
# 历史上两条通道用了两个不同的相对路径：
#   - glog        -> ./logs/server   （logtostderr=true 时其实不落盘）
#   - FileLogSink -> ./server/log    （真正写文件的那条）
# 二者都相对 WorkingDirectory(=current/server) 解析，OTA 切版本即丢。
LEGACY_HIT=""
for d in "/home/radxa/weaknet/current/server/logs" \
         "/home/radxa/weaknet/current/server/log"; do
    if [ -d "$d" ] && [ -n "$(ls -A "$d" 2>/dev/null)" ]; then
        LEGACY_HIT="$d"
        break
    fi
done
if [ -n "$LEGACY_HIT" ]; then
    fail "文件日志：日志仍写进版本目录 $LEGACY_HIT（应落在 data/logs）"
else
    ok "文件日志：未写进版本目录"
fi

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

# Phase 5/6 已移除（2026-11-01）：原先的 "test-events"（事件系统）与
# "test-quality"（网络质量订阅）随客户端订阅族一并删除——服务端自
# 2026-10-19 起停发全部 D-Bus 信号，这两个子命令已不存在。
# 编号不从 4 顺延到 5，是为了不打乱 Phase 7~15 的既有编号（日志、
# 文档、历史排障记录都按这些编号引用）。若要验证等价能力，改用
# Phase 10 的查询类子命令。

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
# 注意（2026-11-01）：原先这里的 "check" 与 "event-types" 随客户端订阅族
# 一并删除（服务端停发信号后它们永远返回 false）。不要再加回来——
# test-client 对未知子命令现在返回非零退出码，加回来会直接判 FAIL。
echo ""
echo "===== Phase 10: 单项命令验证 ====="
run_client_test "ebpf-health" "ebpf-health"
run_client_test "skb-drop" "skb-drop"
run_client_test "dns-stats" "dns-stats"
run_client_test "wifi-loss" "wifi-loss"
run_client_test "http-latency" "http-latency"
run_client_test "profiling" "profiling"
run_client_test "history" "history"
run_client_test "bt-traffic" "bt-traffic"

# ==============================================
# Phase 11: 单元测试（不依赖服务端）
# ==============================================
echo ""
echo "===== Phase 11: 单元测试 ====="
cd /home/radxa/weaknet/server
# 板端只部署 ci.sh 选择打包的测试二进制（当前仅 test/bin/test_ebpf）。
# 55 个 gtest 套件不在板端跑：它们的归属是 x86 本地
# （ctest --test-dir build-x86/server，同时也是 project-profile 的 test-all 命令）
# 与 GitHub Actions。
#
# 这里必须显式判定 test/bin/ 是否为空：bash 的 glob 在无匹配时会把字面量
# "test/bin/*" 传给 for，`[ -x ]` 判假后循环体不执行 —— 整段静默变成一个
# 空操作，而报告里 Phase 11 标题照旧打印，"看起来跑过"。用 nullglob + 计数
# 把"没得跑"变成显式 SKIP，SKIP 会计入统计并出现在汇总里。
shopt -s nullglob
utests=(test/bin/*)
shopt -u nullglob
if [ ${#utests[@]} -eq 0 ]; then
    skip "Phase 11: 板端未部署单元测试二进制（test/bin/ 为空）；gtest 套件在 x86 本地与 CI 运行"
else
    for utest in "${utests[@]}"; do
        if [ -x "$utest" ]; then
            echo "  --- 运行 $utest ---"
            if "./$utest" 2>&1; then
                ok "单元测试: $(basename $utest)"
            else
                fail "单元测试: $(basename $utest)"
            fi
        else
            skip "单元测试（不可执行）: $(basename "$utest")"
        fi
    done
fi
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

# ==============================================
# Phase 14: OTA 升级与 ECDSA 签名回归测试
# ==============================================
echo ""
echo "===== Phase 14: OTA 升级与签名回归测试 ====="
UPDATE_SCRIPT="/home/radxa/weaknet/weaknet-local-update.sh"
if [ -x "$UPDATE_SCRIPT" ]; then
    echo "  --- 14.1: status 查询 ---"
    if /home/radxa/weaknet/client/bin/weaknet-cli update status >/dev/null 2>&1; then
        ok "OTA update status 查询"
    else
        fail "OTA update status 查询"
    fi

    echo "  --- 14.2: 未签名包被拒 (fail-closed) ---"
    TMP_UNSIGNED="/tmp/test-ota-unsigned.tar.zst"
    TMP_DIR="/tmp/test-ota-unsigned-dir"
    rm -rf "$TMP_DIR" "$TMP_UNSIGNED" "${TMP_UNSIGNED}.sig"
    mkdir -p "$TMP_DIR" && echo "1.0.0" > "$TMP_DIR/VERSION"
    (cd "$TMP_DIR" && find . -type f ! -name manifest.sha256 -print0 | sed -z "s|^\./||" | sort -z | xargs -0 sha256sum > manifest.sha256)
    tar --zstd -cf "$TMP_UNSIGNED" -C "$TMP_DIR" .
    if sudo WEAKNET_ROOT=/home/radxa/weaknet "$UPDATE_SCRIPT" verify "$TMP_UNSIGNED" >/dev/null 2>&1; then
        fail "未签名包未能成功拦截 (fail-closed 失败)"
    else
        ok "未签名包拦截 (fail-closed 有效)"
    fi

    echo "  --- 14.3: 伪造私钥签名被拒 ---"
    TMP_ATTACKER_KEY="/tmp/test-attacker-key.pem"
    openssl ecparam -name prime256v1 -genkey -noout -out "$TMP_ATTACKER_KEY" 2>/dev/null || true
    if [ -f "$TMP_ATTACKER_KEY" ]; then
        openssl dgst -sha256 -sign "$TMP_ATTACKER_KEY" -out "${TMP_UNSIGNED}.sig" "$TMP_UNSIGNED" 2>/dev/null || true
        if sudo WEAKNET_ROOT=/home/radxa/weaknet "$UPDATE_SCRIPT" verify "$TMP_UNSIGNED" >/dev/null 2>&1; then
            fail "伪造签名包未能成功拦截"
        else
            ok "伪造私钥签名拦截成功"
        fi
        rm -f "$TMP_ATTACKER_KEY"
    fi
    rm -rf "$TMP_DIR" "$TMP_UNSIGNED" "${TMP_UNSIGNED}.sig"
else
    skip "weaknet-local-update.sh 不存在，跳过 OTA 测试"
fi

# Phase 15: 服务端日志
echo ""
echo "===== Phase 15: 服务端日志（最后 80 行）====="
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
