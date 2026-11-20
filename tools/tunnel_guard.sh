#!/usr/bin/env bash
# =============================================================================
# tunnel_guard.sh —— 反向隧道「转发层」存活守护（云端侧）
# =============================================================================
#
# 解决的问题
# ----------
# `tools/weaknet-tunnel.service` 的 `ServerAliveInterval=10 / ServerAliveCountMax=3`
# 只能发现「板子到云端的 SSH 连接断了」。它**发现不了**另一种更隐蔽的故障：
#
#     SSH 连接还 ESTAB、端口在 listen、TCP 三次握手能成功，
#     但转发通道已经死了，客户端永远等不到 SSH banner。
#
# 2026-11-10 当天出现过三次这种半死状态，表现为
# `Connection timed out during banner exchange`，而云端 `ss -ltn` 显示端口
# 正常 LISTEN、板端 `systemctl status` 显示 `active (running)`，双方都以为
# 自己健康，只能人工介入。
#
# 判据：读到「板端 sshd 的 banner」
# -------------------------------
# 这是本脚本唯一可信的信号，理由与反面样本都来自实测：
#
#   * 不看端口 listen —— 端口在听恰恰是「半死」的特征（假绿）。
#   * 不用 `nc -z` —— TCP 能建连是故障态的正常表现，握手才是分水岭。
#   * **不做 SSH 认证** —— 见下方「2026-11-10 的假阴性」。
#   * 校验 banner 的身份 —— 云端自己的 sshd 是 `OpenSSH_9.6p1 Ubuntu`，
#     板端是 `OpenSSH_8.4p1 Debian-5+deb11u6`。只有读到「Debian 系」的
#     banner 才证明字节真的落到了板端，而不是云端本地某个监听者。
#
# 2026-11-10 的假阴性（本脚本第一版的缺陷，务必不要退回去）
# ---------------------------------------------------------
# 第一版用 `ssh -p PORT ... "${SSH_USER}@${HOST}" true` 做探测，即「完整认证」。
# 这个写法有两个致命问题：
#
#   1. 它从 `127.0.0.1` 直连，**不会命中 `~/.ssh/config` 里的 `Host board`**，
#      因此拿不到 `IdentityFile ~/.ssh/id_board`；`BatchMode=yes` 让认证
#      立刻失败，脚本把「认证失败」误判成「转发层半死」，而随后的自愈动作
#      会去杀掉**健康的**会话 —— 一次误报就能把好隧道放倒。
#   2. 探测把「链路是否通」与「某把钥匙是否可用」耦合在一起。密钥轮换、
#      权限收紧、authorized_keys 调整都会让守护进程误判为链路故障。
#
# 结论：探测只回答「字节能不能从云端 22222 走到板端 22」，不回答「我能不能登录」。
# 后者是 `ssh board` 的事，不是守护进程的事。
#
# 2026-11-11 的假自愈（第二版的缺陷，已修，务必不要退回去）
# ------------------------------------------------------------
# 第二版的探测判据是对的，但**自愈从未真正生效**，且失败方式很隐蔽：
#
#     is_sshd() { exe=$(readlink -f "/proc/$1/exe") ... }   # 无提权
#
# 承载 remote forward 的 sshd 子进程即便以 uid=1000 运行，`/proc/<pid>/exe`
# 的符号链接属主仍是 **root**：
#
#     lrwxrwxrwx 1 root root 0 ... /proc/2814818/exe
#     非 root readlink  → Permission denied（rc=1）
#     sudo -n readlink  → /usr/sbin/sshd
#
# 叠加 `kernel.yama.ptrace_scope=1`，跨进程读 exe 一律要提权。于是 `is_sshd()`
# 对**每一个** pid 都返回非零，`heal()` 一个进程都杀不掉；脚本仍然会打印
# "自行清理"，退出码却是不健康 —— 看起来像"隧道没救活"，实际是识别环节就断了。
# `kill` 同理（root 拥有的 sshd 会被 EPERM 吞掉）。
#
# 现在统一走 `sudo -n`；免密 sudo 不可用时**拒绝动手并明确报错**，不静默降级。
#
# 处置
# ----
# 只有「端口在听 + 连得上 + 等不到 banner」这一种组合才判定为转发层半死，
# 此时找出承载该转发的云端 sshd 会话并清理，让板端 `Restart=always` 重连。
#
# 已知边界
# --------
# 若该端口被**另一条合法隧道**抢占（不是板端这条），本脚本不会去杀它 ——
# 盲杀会形成「你杀它、它重连再抢」的拉锯。这种情形只报告、不自愈，需要人看。
#
# 用法
# ----
#   ./tools/tunnel_guard.sh            # 单次探测 + 自愈
#   ./tools/tunnel_guard.sh --check    # 只探测，不处置（退出码 0/1）
#   ./tools/tunnel_guard.sh --watch    # 常驻，每 60s 探测一次
#
# 退出码：0 健康或已自愈；1 不健康且未自愈（--check 模式下即不健康）。
#
set -uo pipefail

PORT="${TUNNEL_GUARD_PORT:-22222}"
HOST="${TUNNEL_GUARD_HOST:-127.0.0.1}"
# banner 必须匹配这个正则才认作「板端」。默认匹配 Debian 系 OpenSSH，
# 刻意不写死版本号（板端 OTA 升级后版本会变，但发行版标识不变）。
EXPECT_BANNER="${TUNNEL_GUARD_EXPECT_BANNER:-^SSH-2\.0-OpenSSH_.*Debian}"
# 等 banner 的时间。正常 <1s；热点链路 RTT 抖动大，给 5s 足够。
BANNER_WAIT="${TUNNEL_GUARD_BANNER_WAIT:-5}"
WATCH_INTERVAL="${TUNNEL_GUARD_INTERVAL:-60}"

MODE="fix"
case "${1:-}" in
    --check) MODE="check" ;;
    --watch) MODE="watch" ;;
    "") ;;
    *) echo "用法: $0 [--check|--watch]" >&2; exit 2 ;;
esac

log() { printf '[tunnel_guard] %s\n' "$*"; }

# ---------------------------------------------------------------------------
# 提权前缀
#
# 承载转发的 sshd 子进程即使以普通用户身份运行（实测 uid=1000），
# 它的 `/proc/<pid>/exe` 符号链接属主仍是 **root**：
#
#     lrwxrwxrwx 1 root root 0 ... /proc/2814818/exe
#     非 root readlink → Permission denied
#     sudo -n readlink → /usr/sbin/sshd
#
# 叠加 `kernel.yama.ptrace_scope=1`，跨进程读 exe 必须提权。
# `kill` 同理（其它会话的 sshd 可能是 root）。
#
# 这是本脚本第一版自愈**从未生效**的根因：`is_sshd()` 用无提权的 readlink，
# 对每个 pid 都返回非零，于是 `heal()` 一个进程都杀不掉，只打印"无法定位"。
# 表面上"跑了但没救活"，实际是识别环节就断了。
#
# 因此这里统一走 `sudo -n`；免密不可用时保持空串，由下游给出明确告警，
# **绝不静默降级**（宁可不动手，也不能误杀）。
# ---------------------------------------------------------------------------
SUDO=""
if sudo -n true 2>/dev/null; then
    SUDO="sudo -n"
fi

# ---------------------------------------------------------------------------
# 读 banner。这是唯一的健康判据。
#
# 用 bash 内建的 /dev/tcp 而不是 nc：不引入额外依赖，且能精确控制超时。
# `read -r -t` 只在收到换行时返回 0 —— banner 以 \r\n 结尾，正好构成
# 「收到了字节」的 proof；如果对端只建连不说话，read 会超时。
#
# 返回：0 读到 banner；3 连不上；4 连上了但等不到 banner；124 整体超时
# 结果写入 BANNER 变量。
# ---------------------------------------------------------------------------
BANNER=""
read_banner() {
    BANNER=""
    local out rc
    out=$(timeout "$((BANNER_WAIT + 5))" bash -c '
        host="$1"; port="$2"; tmo="$3"
        exec 3<>"/dev/tcp/${host}/${port}" || exit 3
        IFS= read -r -t "${tmo}" line <&3 || exit 4
        printf "%s" "${line}"
    ' _ "${HOST}" "${PORT}" "${BANNER_WAIT}" 2>/dev/null)
    rc=$?
    BANNER="${out}"
    return "${rc}"
}

# 端口是否在 listen（精确匹配端口号，避免 :22222 命中 :222220）
port_listening() {
    ss -ltnH "sport = :${PORT}" 2>/dev/null | grep -q .
}

# ---------------------------------------------------------------------------
# 找出承载该转发会话的云端 sshd 进程。
#
# 识别方法：承载 remote forward 的 sshd 子进程在云端会**监听该端口**，
# 监听 socket 的属主就是那个 sshd 进程。比按用户名匹配精确 —— 同机器上
# 可能有多条 ubuntu 会话，按用户名杀会误伤别人正在用的登录。
#
# 非 root 时 `ss -ltnp` 看不到 users:(...)，因此优先用 sudo -n。
# ---------------------------------------------------------------------------
collect_forward_pids() {
    command -v ss >/dev/null 2>&1 || return 1
    local out pids=""
    # 非 root 时 `ss -ltnp` 看不到 users:(...)，必须提权；否则永远拿不到 pid。
    out=$(${SUDO} ss -ltnpH "sport = :${PORT}" 2>/dev/null) || out=$(ss -ltnpH "sport = :${PORT}" 2>/dev/null)
    pids=$(printf '%s\n' "${out}" | grep -o 'pid=[0-9]*' | cut -d= -f2 | sort -u)
    printf '%s' "${pids}"
}

# 只允许杀 sshd。防止端口被别的进程占用时误杀业务进程。
#
# 提权是**必须**的（原因见上方 SUDO 段）：不加 sudo 时 readlink 对任何
# sshd 都返回 Permission denied，本函数恒假，heal() 就成了空转。
# 若连 sudo -n 都不可用，则拒绝动作并明确报错，不退回无提权路径。
is_sshd() {
    local exe
    if [ -z "${SUDO}" ]; then
        printf '[tunnel_guard] 无法校验 pid=%s 的身份（需要免密 sudo），拒绝终止\n' "$1" >&2
        return 1
    fi
    exe=$(${SUDO} readlink -f "/proc/$1/exe" 2>/dev/null) || {
        printf '[tunnel_guard] 无法读取 pid=%s 的 exe，拒绝终止\n' "$1" >&2
        return 1
    }
    case "${exe}" in
        */sshd) return 0 ;;
        *) printf '[tunnel_guard] 拒绝终止 pid=%s（%s 不是 sshd）\n' "$1" "${exe}" >&2; return 1 ;;
    esac
}

heal() {
    local pids p killed=0
    pids=$(collect_forward_pids)
    if [ -z "${pids}" ]; then
        log "端口在监听但探测失败，且无法定位监听进程 pid（可能需要 root）"
        log "建议用 root 运行本脚本，或手工检查: sudo ss -ltnp 'sport = :${PORT}'"
        return 1
    fi
    for p in ${pids}; do
        is_sshd "${p}" || continue
        log "清理承载转发的 sshd 会话: pid=${p}"
        # 只发 TERM 让 sshd 有机会正常 close；不通再 -9。
        # 同样必须提权：承载转发的 sshd 可能属 root，普通用户 kill 会被 EPERM 吞掉。
        ${SUDO} kill -TERM "${p}" 2>/dev/null || ${SUDO} kill -9 "${p}" 2>/dev/null || {
            log "无法终止 pid=${p}（权限不足？）"
            continue
        }
        killed=1
    done
    [ "${killed}" -eq 1 ] || return 1

    # ---------------------------------------------------------------------
    # 杀完之后的两段等待：30s 正常窗口 + 30s 重杀窗口。
    #
    # 2026-11-18 统计 7 天记录：45 次判定半死、45 次清理，只有 8 次在 30s
    # 内恢复，**37 次落到「30s 内仍未恢复，需人工」**。这 37 次大多不是
    # 真的坏了，而是 30s 不够：板端 `RestartSec=3` + WiFi 重关联 + TCP 握手
    # + 新 sshd 绑定端口，热点链路上经常 40~60s。
    #
    # 所以放宽到 60s。但单纯延长等待会遇到另一种情形：旧会话其实没被杀
    # 干净（sshd 卡在 D 状态或 TERM 被忽略），新会话永远抢不到端口。这种
    # 情况下等 120s 也是白等，必须再杀一次。因此第二段窗口前重新执行一次
    # 清理，只有「清理动作确实又命中过进程」才继续等，避免无脑延长时间。
    # ---------------------------------------------------------------------
    local round i
    for round in 1 2; do
        i=0
        while [ "${i}" -lt 30 ]; do
            sleep 3
            i=$((i + 3))
            if one_round_probe_only; then
                log "已自愈：第 ${round} 段 ${i}s 后转发通道恢复"
                return 0
            fi
        done

        [ "${round}" -eq 1 ] || break

        # 第一段 30s 没恢复：再确认一次是不是还有残留会话在占端口。
        # `collect_forward_pids` 为空 = 端口已释放、纯粹是板端还没连上来，
        # 那就没必要重杀，直接进第二段等待。
        pids=$(collect_forward_pids)
        if [ -n "${pids}" ]; then
            log "30s 未恢复且端口仍被占用，重新清理残留会话: pid=$(echo ${pids})"
            for p in ${pids}; do
                is_sshd "${p}" || continue
                ${SUDO} kill -TERM "${p}" 2>/dev/null || ${SUDO} kill -9 "${p}" 2>/dev/null || true
            done
        else
            log "30s 未恢复但端口已释放，板端尚未重连，继续等待"
        fi
    done

    log "清理后 60s 内仍未恢复，需要人工介入"
    return 1
}

# 只做探测、不处置，避免 heal 内部递归调用 one_round。
one_round_probe_only() {
    local rc
    read_banner
    rc=$?
    [ "${rc}" -eq 0 ] && [[ "${BANNER}" =~ ${EXPECT_BANNER} ]]
}

# ---------------------------------------------------------------------------
# 一轮判定。返回 0 健康（或已自愈）。
# ---------------------------------------------------------------------------
one_round() {
    local rc
    read_banner
    rc=$?

    case "${rc}" in
        0)
            if [[ "${BANNER}" =~ ${EXPECT_BANNER} ]]; then
                log "健康（banner: ${BANNER}）"
                return 0
            fi
            # 读到了 SSH banner，但不是板端 —— 端口被别的东西占了。
            log "端口 ${PORT} 返回了非板端 banner：${BANNER}"
            log "判定为端口被其他监听者占用，不做清理（避免与之拉锯），需人工确认"
            return 1
            ;;
        3)
            if port_listening; then
                log "端口在 listen 但 TCP 连不上（可能只在 IPv6 上监听）"
            else
                log "端口 ${PORT} 未监听：隧道未建立，板端会自行重连"
            fi
            return 1
            ;;
        4)
            if ! port_listening; then
                log "等不到 banner 且端口未监听：隧道未建立（板端会自行重连）"
                return 1
            fi
            log "端口在 listen、TCP 可连、但 ${BANNER_WAIT}s 内等不到 banner：判定为转发层半死"
            if [ "${MODE}" = "check" ]; then
                return 1
            fi
            heal
            return $?
            ;;
        *)
            log "探测异常退出（rc=${rc}）"
            return 1
            ;;
    esac
}

case "${MODE}" in
    check)
        if one_round; then exit 0; fi
        log "不健康"
        exit 1
        ;;
    fix)
        if one_round; then exit 0; fi
        exit 1
        ;;
    watch)
        log "常驻模式启动，端口 ${PORT}，间隔 ${WATCH_INTERVAL}s"
        while true; do
            one_round || true
            sleep "${WATCH_INTERVAL}"
        done
        ;;
esac
