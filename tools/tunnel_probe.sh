#!/bin/bash
# =============================================================================
# weaknet-tunnel-probe.sh —— 板端「转发层」存活探测与自愈（反向隧道的板端兜底）
# =============================================================================
#
# 为什么需要这个脚本
# ------------------
# weaknet-tunnel.service 的 `Restart=always` 只保证「**进程退出**就重启」。
# 它保证不了另一种形态：
#
#     ssh 进程还活着、TCP 连接还 ESTAB、但 22222 的转发通道已经死了。
#
# `ServerAliveInterval=10 / ServerAliveCountMax=3` 也只能发现「SSH 层断了」，
# 发现不了「SSH 层还通、转发层死了」。这种半死状态下，板端会**永远认为
# 自己健康**，直到有人手工重启。
#
# 云端 tools/tunnel_guard.sh 覆盖了「云端视角」的那一半，但它只能在
# 云端观察到「端口在听、握手超时」，处置手段是杀掉会话、指望板端重连。
# 一旦板端自己不再重连（本次排查中观察到清理后 30s 未恢复 37 次），
# 云端就无能为力，只能每 60s 报一次「未监听」。
#
# 本脚本补的是**板端视角**的那一半：从板子自己出发，走完整条链路，
# 验证「字节真的能从云端 22222 回到本机 22」。失败就重启隧道单元。
#
# 判据：为什么必须是「完整往返」
# ------------------------------
# 只看 `systemctl is-active` —— 半死状态它照样是 active（假绿）。
# 只看 `ss -tn` 是否 ESTAB —— 连接在，转发死，同样是假绿。
# 只有「读到**板端自己** sshd 的 banner」才证明整条链路每一跳都活着。
#
# 为什么不能直连 `81.71.76.133:22222`
# ----------------------------------
# **这是本脚本第一版的设计错误，务必不要改回去。**
#
# sshd 的 `GatewayPorts` 默认是 `no`，于是 `-R 22222:127.0.0.1:22` 在云端
# 只把端口绑在**回环**上，实测：
#
#     LISTEN 0 128  127.0.0.1:22222  0.0.0.0:*
#     LISTEN 0 128      [::1]:22222     [::]:*
#
# 公网网卡上**没有**监听者。板端执行
# `exec 3<>/dev/tcp/81.71.76.133/22222` 只会在内核 TCP 超时后失败
# （实测 8s 超时、真实失败点可达数分钟），而且**隧道完全健康时也一样失败**。
# 也就是说那条判据在「健康」和「半死」两种状态下输出相同 —— 它根本不是判据。
#
# 正确做法是 `ssh -W 127.0.0.1:22222`：让**云端 sshd 代连它自己的回环端口**，
# 那条连接正好落在隧道 `-R` 创建的那个监听 socket 上，一路转发回板端 22：
#
#     板端 ssh 客户端 → 云端 :22 → cloud sshd 的 direct-tcpip 通道
#       → 云端 127.0.0.1:22222（隧道 -R 的监听点）
#       → 反向通道 → 板端 127.0.0.1:22 → 板端 sshd 的 banner
#
# 半死状态在这一步的表现是：监听 socket 还在（内核侧由仍存活的 sshd 持有），
# 但反向通道已断，`-W` 要么收到 "connect failed: Connection refused"，
# 要么开通道后永远等不到 banner —— 两种情况都被下面的超时与 banner 校验拦住。
#
# 为什么校验 banner 内容
# ----------------------
# 云端自己的 sshd 是 `OpenSSH_9.6p1 Ubuntu`，板端是 `OpenSSH_8.4p1 Debian`。
# 只判「读到东西」会把「云端本地某进程占着 22222」误判成隧道健康。
# 必须匹配 Debian 系 banner，才证明字节真的落到了板端。
#
# 代价：这条判据把「链路是否通」与「云端密钥是否可用」耦合在一起。
# 这是**可接受**的：隧道本身就用同一把钥匙建连，钥匙坏了隧道必然建不起来，
# 它确实是隧道健康度的一个真实因素。早期云端 guard 之所以避免用 ssh 做探测，
# 是因为它要区分「端口被占」与「隧道没通」；板端这里不需要那个区分。
#
# 用法
# ----
#   weaknet-tunnel-probe.sh            # 单次探测，失败则自愈（供 timer 调用）
#   weaknet-tunnel-probe.sh --check    # 只探测不处置，退出码 0/1
#
# 依赖：ssh（板端已有，隧道本身也用它）、timeout（coreutils）。
# 不需要 nc / netcat。
#
# 退出码：0 健康（或已自愈成功）；1 不健康
# =============================================================================

set -uo pipefail

BANNER=""
BANNER_ERR=""

CLOUD_HOST="${TUNNEL_PROBE_CLOUD_HOST:-81.71.76.133}"
CLOUD_SSH_PORT="${TUNNEL_PROBE_CLOUD_SSH_PORT:-22}"
# 云端**回环**上的隧道端口。不能拿 CLOUD_HOST:CLOUD_PORT 直连，见文件头说明。
CLOUD_PORT="${TUNNEL_PROBE_CLOUD_PORT:-22222}"
CLOUD_USER="${TUNNEL_PROBE_CLOUD_USER:-ubuntu}"
CLOUD_KEY="${TUNNEL_PROBE_CLOUD_KEY:-/home/radxa/.ssh/id_cloud_tunnel}"
UNIT="${TUNNEL_PROBE_UNIT:-weaknet-tunnel.service}"

# 必须匹配「板端」的 banner。默认匹配 Debian 系 OpenSSH。
EXPECT_BANNER="${TUNNEL_PROBE_EXPECT:-OpenSSH_8\.[0-9]+p1 Debian}"

# 等 banner 的时间。正常 <1s；热点链路 RTT 抖动大，给 8s。
BANNER_WAIT="${TUNNEL_PROBE_WAIT:-8}"

# 重启之后的恢复等待窗口。板端 RestartSec=3 + TCP 握手 + WiFi，给 60s。
RECOVER_WAIT="${TUNNEL_PROBE_RECOVER_WAIT:-60}"

MODE="fix"
case "${1:-}" in
    --check) MODE="check" ;;
    ""|--fix) MODE="fix" ;;
    *) echo "用法: $0 [--check]" >&2; exit 2 ;;
esac

log() {
    printf '[tunnel-probe] %s\n' "$*"
}

# ---------------------------------------------------------------------------
# 走完整条链路，读板端 sshd 的 banner。
#
# 用 `ssh -W 127.0.0.1:PORT`：让云端 sshd 代连它自己的回环端口（隧道 -R 的
# 监听点），通道一路转回板端 22。为什么不能直连公网地址见文件头。
#
# 用 `-o BatchMode=yes` 而不是 `-N`：后者不接受 stdin/stdout 隧道语义，
# 而 `-W` 必须靠 stdout 把对端字节桥出来。
#
# stderr 单独收进变量而不是丢弃：半死时 ssh 会打 "connect failed:
# Connection refused"，那是判定「转发层已死」的确凿信号，直接丢掉会退化
# 成「什么都读不到」，与「网络不可达」不可区分。
#
# 三个超时层次对应三种不同的故障，必须分开，否则无法定位：
#   ConnectTimeout  —— 板端到云端 22 的 SSH 建连（WiFi 黑洞 / 路由不可达）
#   -W 通道超时      —— 云端回环端口在听但反向通道已死（半死）
#   banner 读取超时  —— 通道开了但板端 22 不应答
# ---------------------------------------------------------------------------
read_banner() {
    local out_file err_file rc banner

    BANNER=""
    BANNER_ERR=""

    # `-W` 之后的读由 `-o ConnectTimeout` 管不到，所以外层再套一个硬超时。
    # 正常 2s 内完成；给 BANNER_WAIT+6s，留出 SSH 建连 + 通道 + banner 的余量。
    #
    # 输出先落文件再取首行，**不要用 `... | head -1`**：那会让 `$?` 变成
    # head 的状态而不是 timeout/ssh 的状态，一条永远返回 0 的判据——正是
    # 本仓库 pitfalls/ 反复记录的那一类错误。
    out_file=$(mktemp) || return 1
    err_file=$(mktemp) || { rm -f "${out_file}"; return 1; }

    timeout "$((BANNER_WAIT + 6))" \
        ssh -i "${CLOUD_KEY}" \
            -o "ConnectTimeout=${BANNER_WAIT}" \
            -o BatchMode=yes \
            -o StrictHostKeyChecking=accept-new \
            -o LogLevel=ERROR \
            -p "${CLOUD_SSH_PORT}" \
            "${CLOUD_USER}@${CLOUD_HOST}" \
            -W "127.0.0.1:${CLOUD_PORT}" < /dev/null >"${out_file}" 2>"${err_file}"
    rc=$?

    banner=$(head -1 "${out_file}" 2>/dev/null | tr -d '\r')
    BANNER_ERR=$(head -3 "${err_file}" 2>/dev/null | tr '\n' ' ')
    rm -f "${out_file}" "${err_file}" 2>/dev/null

    if [ -n "${banner}" ]; then
        BANNER="${banner}"
        return 0
    fi
    # 通道开了但一个字节都没回来，与「连不上」不是同一个故障，要分开报。
    [ "${rc}" -eq 0 ] && rc=4
    return "${rc}"
}

# 一次只判健康与否，不处置。
probe_only() {
    read_banner
    local rc=$?
    [ "$rc" -eq 0 ] || return 1
    [[ "${BANNER}" =~ ${EXPECT_BANNER} ]] || return 1
    return 0
}

# ---------------------------------------------------------------------------
# 自愈：重启隧道单元，然后在恢复窗口内反复探测。
# ---------------------------------------------------------------------------
heal() {
    log "判定为转发层不健康，重启 ${UNIT}"
    systemctl restart "${UNIT}" 2>/dev/null
    local rc=$?
    if [ "$rc" -ne 0 ]; then
        log "systemctl restart 失败（rc=${rc}）：检查是否有权限或单元是否存在"
        return 1
    fi

    local elapsed=0 interval=5
    while [ "$elapsed" -lt "$RECOVER_WAIT" ]; do
        sleep "$interval"
        elapsed=$((elapsed + interval))
        if probe_only; then
            log "已自愈：${elapsed}s 后转发通道恢复（banner: ${BANNER}）"
            return 0
        fi
    done

    log "重启后 ${RECOVER_WAIT}s 仍未恢复，需要人工介入"
    return 1
}

# ---------------------------------------------------------------------------
# 一轮判定。返回 0 健康（或已自愈）。
# ---------------------------------------------------------------------------
one_round() {
    read_banner
    local rc=$?

    if [ "$rc" -eq 0 ]; then
        if [[ "${BANNER}" =~ ${EXPECT_BANNER} ]]; then
            log "健康（banner: ${BANNER}）"
            return 0
        fi
        log "读到了非板端 banner：${BANNER}"
        log "端口被其它监听者占用，不做自愈（避免与之拉锯），需人工确认"
        return 1
    fi

    case "$rc" in
        124) log "探测整体超时（$((BANNER_WAIT + 6))s）：链路黑洞或对端无响应" ;;
        255)
            # ssh 自身的失败。半死时这里是 `connect failed: Connection refused`：
            # 云端回环端口听起来还在，但反向通道已断，转发到板端这一步失败。
            if [[ "${BANNER_ERR}" == *refused* ]]; then
                log "云端回环 ${CLOUD_PORT} 拒绝连接：反向转发通道已死（典型半死）"
            elif [[ "${BANNER_ERR}" == *"Permission denied"* ]]; then
                log "板端密钥被云端拒绝：${BANNER_ERR}"
                log "这不是隧道故障，重启隧道无意义，需人工修密钥"
                return 1
            elif [ -n "${BANNER_ERR}" ]; then
                log "ssh 失败：${BANNER_ERR}"
            else
                log "ssh 失败（rc=255，无 stderr）"
            fi
            ;;
        4) log "通道已开但 ${BANNER_WAIT}s 内等不到 banner：转发层半死" ;;
        *) log "探测异常退出（rc=${rc}${BANNER_ERR:+，${BANNER_ERR}}）" ;;
    esac

    if [ "${MODE}" = "check" ]; then
        return 1
    fi
    heal
}

if one_round; then
    exit 0
fi
log "不健康"
exit 1
