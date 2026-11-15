#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# weaknet-wdt-takeover.sh - 看门狗接管 / 归还（方案 B 的安全网）
#
# 为什么需要这个脚本而不是直接写几条命令：
#   接管过程有一个窗口，期间系统没有任何硬件看门狗在保护。顺序一旦
#   颠倒（先 unbind 再加载新驱动），中间就是「无设备、无日志、无保护」
#   的真空期；此时任何故障都不会被复位兜住，而且 dmesg 里什么都不留
#   （sunxi_wdt_stop() 完全静默）。
#
# 因此顺序不可颠倒：
#   1. insmod weaknet_wdt.ko
#   2. 确认新设备节点存在         <- 不存在就 exit，绝不 unbind
#   3. unbind sunxi_wdt
#   4. 任一失败 -> 立刻 bind 回来
#
# 为什么 unbind 之后设备编号不会自动变成 watchdog0：
#   /dev/watchdog0 是 sunxi_wdt 注册时分配的编号。unbind 只让该设备
#   消失，编号不会回收给后注册的驱动。本模块注册时 watchdog0 已被占用，
#   因此拿到的是 watchdog1，unbind 之后仍然是 watchdog1。
#   这一点已板端实测确认（2026-11-14）：unbind 后只剩 /dev/watchdog1。
#
# 用法：
#   weaknet-wdt-takeover.sh status      # 只读：报告当前状态
#   weaknet-wdt-takeover.sh take        # 接管（unbind sunxi_wdt）
#   weaknet-wdt-takeover.sh give-back   # 归还（unbind 我们 + bind sunxi_wdt）
#
# 退出码：0 成功，1 失败（失败时已尽力回滚）

set -uo pipefail

KMOD_NAME="weaknet_wdt"
SYSFS_DRV="/sys/bus/platform/drivers/sunxi-wdt"
DT_DEV="2050000.watchdog"
NEW_DEV="/dev/watchdog1"
ORIG_DEV="/dev/watchdog0"

# 已触发标记（与 weaknet-health-guard.cpp 的 kTripFlag 同一文件）。
# 里存的是写入时的 boot_id；与当前 boot_id 相同 = 本次启动已判定不健康。
TRIP_FLAG="/var/lib/weaknet/health-guard-tripped"
BOOT_ID_FILE="/proc/sys/kernel/random/boot_id"

# .ko 路径：经 current 软链寻址，OTA 翻链后自动跟着翻
KO_PATH="${WEAKNET_KO_PATH:-/home/radxa/weaknet/current/kmod/weaknet_wdt.ko}"

log() { echo "[wdt] $*"; }
err() { echo "[wdt] 错误: $*" >&2; }

# 模块是否已加载
kmod_loaded() {
    [ -d "/sys/module/${KMOD_NAME}" ]
}

# sunxi_wdt 是否仍绑定着 DT 设备
sunxi_bound() {
    [ -e "${SYSFS_DRV}/${DT_DEV}" ]
}

# 本次启动是否已触发过健康看门狗复位。
#
# 为什么脚本也要判：guard 判定不健康后停止喂狗并退出，此时**任何**重新
# 打开 /dev/watchdog1 的动作都会触发 .start（使能 + 喂狗），把已经决定
# 的复位静默取消。反向 bind sunxi_wdt 同样危险：本模块设了
# watchdog_stop_on_unregister，rmmod 会调用 .stop 关掉硬件看门狗。
# 所以「已触发」状态下，接管与归还都必须拒绝执行，必须由人先排查再删标记。
tripped_this_boot() {
    [ -f "$TRIP_FLAG" ] || return 1
    [ -r "$BOOT_ID_FILE" ] || return 1
    local saved cur
    saved="$(tr -d '\r\n' < "$TRIP_FLAG" 2>/dev/null)"
    cur="$(tr -d '\r\n' < "$BOOT_ID_FILE" 2>/dev/null)"
    [ -n "$saved" ] && [ "$saved" = "$cur" ]
}

# guard 是否**走完了正常停机路径**（写了 'V' 停用硬件）。
#
# 背景（2026-11-15 板端实测发现）：只靠 tripped_this_boot 不足以防住
# 「复位被静默取消」。三种退出无法写入 trip 标记：
#
#   1. 被 SIGKILL / SIGSEGV / OOM 杀死  —— 没机会跑到 RecordTrip()
#   2. 退出码 1（open 失败、喂狗写失败） —— 代码里本就不写标记
#   3. 判据失败但 RecordTrip() 写文件失败（磁盘满）
#
# 而 systemd 在这三种情况下**都会执行 ExecStopPost**（已用无害探针单元
# 实测：kill -9 后 STOPPOST service_result=signal exit_code=killed）。
# 于是 give-back 照常 rmmod → watchdog_stop_on_unregister → .stop →
# 硬件看门狗被关掉 → 板子永不复位，故障还在、保护没了。
#
# 判据用 systemd 注入的 $SERVICE_RESULT / $EXIT_CODE / $EXIT_STATUS。
# **这三个变量的实际取值与直觉不符，必须按实测写**（2026-11-15 板端
# 用无害探针单元 systemd 247 实测）：
#
#   场景                          SERVICE_RESULT  EXIT_CODE  EXIT_STATUS
#   ----------------------------  --------------  ---------  -----------
#   systemctl stop（优雅 SIGTERM） success         killed     TERM
#   KillSignal=SIGKILL 或 kill -9  signal          killed     9
#   主进程 return 99              exit-code       exited     99
#   TimeoutStopSec 超时           timeout         killed     9
#
# 注意第一行：正常 stop 时 EXIT_CODE 是 'killed'、EXIT_STATUS 是 'TERM'
# **而不是 0**。若按「EXIT_STATUS != 0 就拒绝」实现，每次正常停止服务
# 都会被拒绝归还，模块与设备永久残留 —— 这是本轮实测抓到的真实 bug。
#
# 因此唯一可靠的主判据是 SERVICE_RESULT：
#   success → guard 走完了优雅路径（写了 'V'），允许归还
#   其余     → 复位可能已排定，拒绝归还
#
# 为什么不看 EXIT_CODE/EXIT_STATUS：它们的形态随 systemd 版本与信号
# 变化（数字或符号名），无法用固定比较式可靠判断。
guard_exited_uncleanly() {
    local result="${SERVICE_RESULT:-}"

    # 变量缺失 = 手工执行（运维显式动作），放行
    [ -z "$result" ] && return 1

    # 只有 success 表示主进程按自己的正常路径退出
    [ "$result" = "success" ] && return 1

    # exit-code 且 EXIT_STATUS=0 也属正常退出（部分版本不置 success）
    if [ "$result" = "exit-code" ] && [ "${EXIT_STATUS:-}" = "0" ]; then
        return 1
    fi

    return 0
}

do_status() {
    echo "=== 模块 ==="
    if kmod_loaded; then
        log "${KMOD_NAME} 已加载"
    else
        log "${KMOD_NAME} 未加载"
    fi

    echo "=== 设备节点 ==="
    for d in /dev/watchdog*; do
        [ -e "$d" ] || continue
        echo "  $(ls -l "$d")"
    done

    echo "=== sunxi_wdt 绑定状态 ==="
    if sunxi_bound; then
        log "已绑定 ${DT_DEV}"
    else
        log "未绑定（已 unbind）"
    fi

    echo "=== 驱动持有的 watchdog 设备 ==="
    for c in /sys/class/watchdog/watchdog*; do
        [ -e "$c" ] || continue
        local drv
        drv=$(basename "$(readlink -f "$c/device/driver" 2>/dev/null)" 2>/dev/null)
        echo "  $(basename "$c") -> ${drv:-<none>}"
    done

    echo "=== iomem ==="
    grep -i "2050000" /proc/iomem 2>/dev/null || echo "  (无占用记录)"

    echo "=== 触发状态 ==="
    if tripped_this_boot; then
        log "本次启动已触发健康看门狗复位（${TRIP_FLAG} 与当前 boot_id 一致）"
        log "接管/归还均被拒绝；排查故障后删除该文件并重启服务"
    else
        log "未触发"
    fi

    echo "=== 维护标记 ==="
    if [ -e /var/lib/weaknet/health-guard-maintenance ]; then
        log "存在（判据暂停中，30 分钟内有效）"
    else
        log "不存在"
    fi
}

do_take() {
    log "开始接管"

    # 已触发就什么都不做：见 tripped_this_boot 注释。
    # 此时保持 sunxi_wdt 原样，让板子在复位前处于最初状态。
    if tripped_this_boot; then
        err "本次启动已触发健康看门狗，拒绝接管（复位已排定）"
        err "排查故障后：rm -f ${TRIP_FLAG} 再启动本服务"
        return 0
    fi

    # 幂等：已经接管过就直接返回
    if kmod_loaded && ! sunxi_bound && [ -e "$NEW_DEV" ]; then
        log "已是接管状态，无需重复操作"
        return 0
    fi

    # ---- 步骤 1：先加载自己的模块，此时 sunxi_wdt 仍在保护系统 ----
    if ! kmod_loaded; then
        if [ ! -f "$KO_PATH" ]; then
            err "模块不存在: $KO_PATH"
            err "（未做任何改动，sunxi_wdt 仍在保护系统）"
            return 1
        fi
        if ! insmod "$KO_PATH"; then
            err "insmod 失败，未做任何改动"
            return 1
        fi
        log "insmod 成功"
    else
        log "模块已加载，跳过 insmod"
    fi

    # ---- 步骤 2：确认新设备可用，这是唯一允许继续的安全闸门 ----
    if [ ! -c "$NEW_DEV" ]; then
        err "$NEW_DEV 未出现，放弃接管（保持 sunxi_wdt 不动）"
        rmmod "$KMOD_NAME" 2>/dev/null
        return 1
    fi
    log "确认 $NEW_DEV 存在"

    # ---- 步骤 3：撤掉旧保护（此时两套驱动短暂共存于同一物理区间） ----
    if sunxi_bound; then
        if ! echo "$DT_DEV" > "${SYSFS_DRV}/unbind" 2>/dev/null; then
            err "unbind 失败，回滚：卸载我们自己的模块"
            rmmod "$KMOD_NAME" 2>/dev/null
            return 1
        fi
        log "已 unbind sunxi_wdt"

        # ---- 步骤 4：验证旧设备确实消失；若没有，说明 unbind 未生效 ----
        if [ -e "$ORIG_DEV" ]; then
            err "$ORIG_DEV 仍然存在，unbind 未生效，回滚"
            echo "$DT_DEV" > "${SYSFS_DRV}/bind" 2>/dev/null
            rmmod "$KMOD_NAME" 2>/dev/null
            return 1
        fi
    fi

    # ---- 最终校验 ----
    if [ ! -c "$NEW_DEV" ]; then
        err "最终校验失败：$NEW_DEV 消失，尝试完整回滚"
        do_give_back >/dev/null 2>&1
        return 1
    fi

    log "接管完成：喂狗入口 ${NEW_DEV}"
    return 0
}

do_give_back() {
    log "开始归还"

    # 已触发时拒绝归还：rmmod 会经 watchdog_stop_on_unregister 调用 .stop，
    # bind 回 sunxi_wdt 又会让本模块退场——两者都会关掉硬件看门狗，
    # 把已经判定的复位取消掉。必须由人先删标记（见 tripped_this_boot）。
    if tripped_this_boot; then
        err "本次启动已触发健康看门狗，拒绝归还——归还等于取消复位"
        err "如需强行归还（例如故障已确认是误判）：rm -f ${TRIP_FLAG} 后重试"
        return 0
    fi

    # 第二道闸：guard 没有走完正常停机路径（崩溃 / 被 kill -9 / 非零退出）
    # 时，硬件仍在倒计时，归还等于取消复位。见 guard_exited_uncleanly。
    #
    # 为什么不能只靠上面的 trip 标记：被杀时根本来不及写标记。
    if guard_exited_uncleanly; then
        err "guard 未正常退出（SERVICE_RESULT=${SERVICE_RESULT:-?} EXIT_CODE=${EXIT_CODE:-?}），拒绝归还"
        err "硬件看门狗仍在倒计时，复位将按计划发生；排查后重启服务即可"
        err "若确认是误判并希望强行归还：SERVICE_RESULT= EXIT_CODE= $0 give-back"
        return 0
    fi

    # 先 bind 回原驱动（恢复内建保护），再卸载我们的模块。
    # 顺序不能反：先 rmmod 会让新设备先行消失，中间又出现真空期。
    if ! sunxi_bound; then
        if echo "$DT_DEV" > "${SYSFS_DRV}/bind" 2>/dev/null; then
            log "已 bind 回 sunxi_wdt"
        else
            err "bind 回 sunxi_wdt 失败（重启后内建驱动会重新 probe）"
        fi
    else
        log "sunxi_wdt 已绑定，跳过 bind"
    fi

    # 等设备节点出现（bind 后 udev/内核同步创建）
    for _ in $(seq 1 20); do
        [ -c "$ORIG_DEV" ] && break
        sleep 0.1
    done

    if kmod_loaded; then
        if rmmod "$KMOD_NAME"; then
            log "已卸载 ${KMOD_NAME}"
        else
            err "rmmod 失败（可能仍有进程持有设备）"
            return 1
        fi
    fi

    if [ -c "$ORIG_DEV" ]; then
        log "归还完成：${ORIG_DEV} 已恢复"
        return 0
    fi

    err "归还后 ${ORIG_DEV} 未恢复"
    return 1
}

case "${1:-status}" in
    status)    do_status ;;
    take)      do_take ;;
    give-back) do_give_back ;;
    *)
        echo "用法: $0 {status|take|give-back}" >&2
        exit 2
        ;;
esac
