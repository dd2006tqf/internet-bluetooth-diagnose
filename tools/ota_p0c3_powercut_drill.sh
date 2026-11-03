#!/usr/bin/env bash
# P0-C3 真实断电测试台 —— 板端执行
#
# 目的：验证「写在关键窗口中途掉电」时，OTA 事务状态的物理持久性与开机自愈。
#       这与 P0-C2 的 `reboot` 有本质区别：`reboot` 是有序关机，内核会把页缓存
#       刷到设备；断电则可能在 `fsync` 返回后仍有数据留在设备写缓存里
#       （板端 /sys/block/sda/queue/write_cache = "write back"）。
#
# 隔离原则（沿用 P0-C2 的教训）：
#   * 独立 root      : /var/lib/p0c3-sb/root   （生产是 /home/radxa/weaknet）
#   * 独立 state-dir : /var/lib/p0c3-sb/state  （生产是 /var/lib/weaknet-ota）
#   * 不装任何开机自启单元 —— 恢复判定由本脚本在重新上电后手工驱动，
#     避免沙盒恢复器接触真实 systemctl 单元（P0-C2 踩过：enter_rescue 会
#     stop+mask 真实单元）。
#
# 用法：
#   sudo bash ota_p0c3_powercut_drill.sh setup <scenario>
#   sudo bash ota_p0c3_powercut_drill.sh status
#   sudo bash ota_p0c3_powercut_drill.sh verify
#   sudo bash ota_p0c3_powercut_drill.sh cleanup
#
# scenario 取值：
#   trial            TRIAL 刚建立后断电（最核心：状态是否持久）
#   arm-switch       arm-switch 后、do-switch 前断电
#   do-switch        do-switch 完成瞬间断电
#   commit-immediate TRIAL 健康转正后立即断电
set -eu

SB=/var/lib/p0c3-sb
LIB=/usr/local/lib/p0c3-sb
CTL="$LIB/ctl"
STATE="$SB/state"
ROOT="$SB/root"

say() { echo "[p0c3] $*"; }

need_root() {
    [ "$(id -u)" -eq 0 ] || { echo "必须用 sudo 运行" >&2; exit 1; }
}

ctl() {
    python3 "$CTL" --root "$ROOT" --state-dir "$STATE" "$@"
}

setup_base() {
    say "=== 准备沙盒（持久目录，不会被开机清理）==="
    rm -rf "$SB" "$LIB"
    install -d -m 0700 "$SB" "$STATE"
    install -d -m 0755 "$LIB"

    # 控制器从生产安装位置复制 —— 复制而非引用，避免断电实验影响生产文件
    install -m 0755 /usr/local/lib/weaknet/weaknet-ota-ctl "$CTL"

    for v in 1.0.0 1.1.0; do
        install -d -m 0755 "$ROOT/releases/$v/server/bin"
        printf '%s\n' "$v" > "$ROOT/releases/$v/VERSION"
        printf '#!/bin/sh\necho p0c3-sandbox-%s\n' "$v" \
            > "$ROOT/releases/$v/server/bin/weaknet-dbus-server"
        chmod +x "$ROOT/releases/$v/server/bin/weaknet-dbus-server"
    done
    ln -sfn releases/1.0.0 "$ROOT/current"
    say "  current -> $(readlink -f "$ROOT/current")"
}

write_marker() {
    # 断电前最后写入的标记：用于判断"断电发生在哪一步之后"。
    # 必须 fsync，否则标记本身也可能丢失，导致误判。
    python3 - "$STATE" "$1" <<'PY'
import os, sys, time
state_dir, tag = sys.argv[1], sys.argv[2]
path = os.path.join(state_dir, "powercut-marker.txt")
with open(path, "w", encoding="utf-8") as fh:
    fh.write("%s %s\n" % (tag, time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())))
    fh.flush()
    os.fsync(fh.fileno())
d = os.open(state_dir, os.O_RDONLY)
os.fsync(d)
os.close(d)
PY
    say "  已落盘断电标记：$1"
}

scenario_trial() {
    say "=== 场景 trial：TRIAL 建立后立即断电 ==="
    ctl begin --from 1.0.0 --to 1.1.0 --rollback-target 1.0.0 \
        --policy '{"health_timeout": 600, "health_poll_s": 10}' >/dev/null
    ctl materialize >/dev/null
    ctl arm-switch >/dev/null
    ctl do-switch >/dev/null
    write_marker "trial-established"
    say ""
    say "现在事务处于 TRIAL。请**立即断电**。"
    say "期望（重新上电后 verify）："
    say "  * transaction.json 完整 -> phase=TRIAL, boot_attempts 保持"
    say "  * 文件损坏 -> 状态不可读，这是真实数据丢失，要如实记录"
    ctl status
}

scenario_arm_switch() {
    say "=== 场景 arm-switch：切换意图已落盘但指针未翻转时断电 ==="
    ctl begin --from 1.0.0 --to 1.1.0 --rollback-target 1.0.0 \
        --policy '{"health_timeout": 600, "health_poll_s": 10}' >/dev/null
    ctl materialize >/dev/null
    ctl arm-switch >/dev/null
    write_marker "arm-switch-done"
    say ""
    say "现在事务处于 SWITCH_PENDING、指针仍指 1.0.0。请**立即断电**。"
    say "期望：phase=SWITCH_PENDING，current 仍为 1.0.0"
    ctl status
}

scenario_do_switch() {
    say "=== 场景 do-switch：指针翻转完成瞬间断电 ==="
    ctl begin --from 1.0.0 --to 1.1.0 --rollback-target 1.0.0 \
        --policy '{"health_timeout": 600, "health_poll_s": 10}' >/dev/null
    ctl materialize >/dev/null
    ctl arm-switch >/dev/null
    ctl do-switch >/dev/null
    write_marker "do-switch-done"
    say ""
    say "现在指针已翻到 1.1.0、事务处于 TRIAL。请**立即断电**。"
    say "期望：current 只可能是 1.0.0 或 1.1.0（原子），不会出现第三值；"
    say "      phase 与 current 必须自洽（TRIAL 要求 current=1.1.0）"
    ctl status
    say "  current -> $(readlink -f "$ROOT/current")"
}

scenario_commit() {
    say "=== 场景 commit-immediate：转正后立即断电 ==="
    ctl begin --from 1.0.0 --to 1.1.0 --rollback-target 1.0.0 \
        --policy '{"health_timeout": 600, "health_poll_s": 10}' >/dev/null
    ctl materialize >/dev/null
    ctl arm-switch >/dev/null
    ctl do-switch >/dev/null
    ctl commit >/dev/null
    write_marker "commit-done"
    say ""
    say "现在事务已 COMMITTED。请**立即断电**。"
    say "期望：phase=COMMITTED, stable_version=1.1.0（转正不丢失）"
    ctl status
}

cmd_status() {
    say "=== 沙盒当前状态 ==="
    [ -d "$STATE" ] || { say "沙盒不存在（先 setup）"; exit 1; }
    ctl status || say "  ! 状态不可读（可能是断电损坏）"
    say "  current -> $(readlink -f "$ROOT/current" 2>/dev/null || echo '(缺失)')"
    if [ -f "$STATE/powercut-marker.txt" ]; then
        say "  断电标记：$(cat "$STATE/powercut-marker.txt")"
    else
        say "  断电标记：（无 —— 标记本身丢了，说明目录元数据未落盘）"
    fi
    say "  当前 boot_id：$(cat /proc/sys/kernel/random/boot_id)"
    say "  开机时间：$(uptime -s)"
}

cmd_verify() {
    say "=== 断电后校验 ==="
    [ -d "$STATE" ] || { say "沙盒目录不存在 —— 最严重的数据丢失"; exit 1; }

    local marker="(无)"
    [ -f "$STATE/powercut-marker.txt" ] && marker=$(cat "$STATE/powercut-marker.txt")

    say "  断电标记：$marker"
    say "  当前 boot_id：$(cat /proc/sys/kernel/random/boot_id)"
    say "  开机时间：$(uptime -s)"
    say ""

    say "--- transaction.json 原始内容 ---"
    if [ -f "$STATE/transaction.json" ]; then
        cat "$STATE/transaction.json"
    else
        say "  ! transaction.json 不存在 —— 状态完全丢失"
    fi
    say ""

    say "--- 控制器视角 ---"
    if ctl status >/dev/null 2>&1; then
        say "  OK 状态可解析"
    else
        say "  BAD 状态不可解析（断电导致损坏）"
    fi
    say ""

    say "--- reconcile 判定（只读）---"
    ctl reconcile --dry-run 2>&1 || say "  （reconcile 返回非零，见上）"
    say ""

    say "--- current 指针 ---"
    say "  解析值：$(readlink -f "$ROOT/current" 2>/dev/null || echo '(缺失)')"
    say "  原始 target：$(readlink "$ROOT/current" 2>/dev/null || echo '(缺失)')"
}

cmd_cleanup() {
    say "=== 清理沙盒 ==="
    rm -rf "$SB" "$LIB"
    say "  已删除 $SB 与 $LIB"
}

usage() {
    sed -n '16,27p' "$0"
    exit 1
}

need_root
[ $# -ge 1 ] || usage
cmd="$1"; shift || true

case "$cmd" in
    setup)
        [ $# -ge 1 ] || { echo "setup 需要 scenario 参数" >&2; usage; }
        setup_base
        case "$1" in
            trial)            scenario_trial ;;
            arm-switch)       scenario_arm_switch ;;
            do-switch)        scenario_do_switch ;;
            commit-immediate) scenario_commit ;;
            *) echo "未知 scenario: $1" >&2; usage ;;
        esac
        ;;
    status)  cmd_status ;;
    verify)  cmd_verify ;;
    cleanup) cmd_cleanup ;;
    *) usage ;;
esac
