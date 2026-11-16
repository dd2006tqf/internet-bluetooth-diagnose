#!/usr/bin/env bash
# board_vin_stage_probe.sh — VIN 管线「阶段到达情况」一次性探针（宿主机侧执行）
#
# 用途：回答一个只能用真机回答的问题——STREAMON 之后，TDM/CSI/MIPI 各阶段的
#      函数到底被调用了几次？零次还是 N 次？
#
# 为什么需要它：sunxi-vin 大量使用「条件不满足就 return 0」的写法，应用层看到的
#      全是成功。返回值不可信，只能去数「函数有没有真的跑过」。
#
# 安全性（三条硬规则，本脚本自己遵守）：
#   1. 绝不读 /sys/kernel/debug/mpp/mipi —— open() 里直接 readl 已 gated 的
#      PHY 寄存器，配合 PANIC_ON_OOPS=y + PANIC_TIMEOUT=0，一读整板冻结且永不
#      重启，timeout 也救不了。见 openspec/pitfalls/mpp-mipi-debugfs-panics.md
#   2. 绝不对 /dev/video* 用 lsof / fuser —— 已两次导致整板失联。
#      见 openspec/pitfalls/lsof-v4l2-kills-board.md
#   3. 停/重启 weaknet-server 之前必须建 health-guard-maintenance 标记，
#      否则硬件看门狗 16 秒内整机复位。本脚本用 trap 保证标记一定被删。
#
# 只做四件事：kprobe 计数、读 /proc/interrupts、读 /sys/kernel/debug/mpp/vi、
# 重启一次 weaknet-server 触发 open。不改任何驱动状态。
#
# 用法：
#   ./tools/board_vin_stage_probe.sh              # 完整两轮（open #1 / open #2）
#   ./tools/board_vin_stage_probe.sh --dry-run    # 只预检，不装探针、不动服务
#   ./tools/board_vin_stage_probe.sh --no-restart # 装好探针后等 40s，由你手动重启

set -uo pipefail

BOARD="${BOARD:-board}"
DRY_RUN=0
NO_RESTART=0
for a in "$@"; do
    case "$a" in
        --dry-run)    DRY_RUN=1 ;;
        --no-restart) NO_RESTART=1 ;;
        *) echo "未知参数: $a" >&2; exit 2 ;;
    esac
done

MAINT=/var/lib/weaknet/health-guard-maintenance
TRACE=""
WIN=40          # 每轮观察窗口（秒）
PROBES_OK=()
PROBES_FAIL=()

say()  { printf '\n\033[1m== %s ==\033[0m\n' "$*"; }
note() { printf '   %s\n' "$*"; }
warn() { printf '   \033[33m! %s\033[0m\n' "$*"; }

board() { ssh -o ConnectTimeout=8 "$BOARD" "$@"; }

cleanup() {
    if [ -n "$TRACE" ]; then
        board "sudo -n sh -c 'echo 0 > $TRACE/events/kprobes/enable 2>/dev/null; echo > $TRACE/kprobe_events 2>/dev/null'" >/dev/null 2>&1
    fi
    # 维护标记必须删掉，否则守卫会永远处于维护模式（不再按健康判据喂狗）
    board "sudo -n rm -f $MAINT" >/dev/null 2>&1
}
trap cleanup EXIT

# ------------------------------------------------------------------ 1. 预检
say "1. 预检"

if ! board 'true' 2>/dev/null; then
    echo "开发板不可达（ssh $BOARD）。本脚本需要板子在线。" >&2
    exit 1
fi
note "ssh $BOARD                    OK"
note "$(board 'uname -r' | tr -d '\r')"

if board 'test -d /sys/kernel/tracing'; then
    TRACE=/sys/kernel/tracing
elif board 'test -d /sys/kernel/debug/tracing'; then
    TRACE=/sys/kernel/debug/tracing
else
    echo "板端没有 tracefs（需要 CONFIG_FTRACE + CONFIG_KPROBE_EVENTS），本脚本无法工作。" >&2
    exit 1
fi
note "tracefs = $TRACE"

if ! board "sudo -n test -w $TRACE/kprobe_events" 2>/dev/null; then
    echo "无法写 $TRACE/kprobe_events（可能未编 CONFIG_KPROBE_EVENTS）。" >&2
    exit 1
fi
note "kprobe_events 可写           OK"

# DT 里的 work_mode：唯一能直接定死 ONLINE/OFFLINE 的证据。
say "2. 设备树 work_mode（只读）"
board 'for d in /proc/device-tree/soc*/*tdm* /proc/device-tree/*tdm*; do
           [ -d "$d" ] || continue
           echo "node: $d"
           for p in work_mode device_id; do
               if [ -e "$d/$p" ]; then
                   printf "  %-12s = %s\n" "$p" "$(hexdump -e "1/4 \"%u\"" "$d/$p" 2>/dev/null)"
               else
                   printf "  %-12s = (absent)\n" "$p"
               fi
           done
       done' 2>/dev/null | sed 's/^/   /'
note "work_mode 缺失 → of_property_read_u32 不写目标 → clamp(0xff,0,1) = 1 = TDM_OFFLINE"
note "work_mode == 0 → TDM_ONLINE"

if [ "$DRY_RUN" = 1 ]; then
    say "dry-run 结束（未装探针、未动服务）"
    exit 0
fi

# -------------------------------------------------------------- 3. 布探针
say "3. 安装 kprobe（按阶段）"

# 每行：<事件名> <内核函数> <它代表哪个阶段真的被执行了>
# static 函数若被内联进调用者，kprobe 注册会失败。失败本身必须如实记录，
# 绝不能把「探针没装上」读成「函数没被调用」。
STAGES=(
  "vin_s_stream_in   sunxi_tdm_subdev_s_stream   TDM子设备收到s_stream"
  "vin_tdm_top       sunxi_tdm_top_s_stream      TDM top配置+中断使能"
  "vin_tdm_rxen      csic_tdm_rx_enable          TDM接收通道使能"
  "vin_tdm_rxcap     csic_tdm_rx_cap_enable      TDM接收cap使能"
  "vin_tdm_isr       tdm_isr                     TDM中断服务"
  "vin_tdm_reset_wq  __sunxi_tdm_reset_v2_handle TDM复位workqueue"
  "vin_csi_sstream   sunxi_csi_subdev_s_stream   CSI子设备s_stream"
  "vin_prs_capstart  csic_prs_capture_start      PARSER开始采集"
  "vin_mipi_sstream  sunxi_mipi_subdev_s_stream  MIPI子设备s_stream"
  "vin_mipi_mbuscfg  sunxi_mipi_s_mbus_config    图遍历走到MIPI设mbus"
  "vin_combo_csi     combo_csi_init              MIPIPHY初始化入口"
  "vin_sensor_sstrm  sensor_s_stream             sensor出口开始/停止"
)

board "sudo -n sh -c 'test -e $TRACE/kprobe_events && echo > $TRACE/kprobe_events'" >/dev/null 2>&1

for row in "${STAGES[@]}"; do
    set -- $row
    ev="$1"; fn="$2"; desc="$3"
    if board "sudo -n sh -c 'echo \"p:$ev $fn\" >> $TRACE/kprobe_events'" 2>/dev/null; then
        PROBES_OK+=("$ev|$fn|$desc")
        note "OK   $fn  ($desc)"
    else
        PROBES_FAIL+=("$ev|$fn|$desc")
        warn "FAIL $fn  — 装不上，本轮不能据此判断该阶段是否执行"
    fi
done

if [ ${#PROBES_OK[@]} -eq 0 ]; then
    echo "一个探针都没装上，退出。" >&2
    exit 1
fi

board "sudo -n sh -c 'echo 8192 > $TRACE/buffer_size_kb 2>/dev/null; echo 1 > $TRACE/events/kprobes/enable'" >/dev/null 2>&1

# ------------------------------------------------------------------ 4. 两轮
# 每轮：清 trace → 触发一次 open（重启服务）→ 观察窗口 → 收计数。
#
# 为什么用「重启服务」而不是自己开 /dev/video0：设备被 weaknet-server 独占，
# 另开进程只会拿到 EBUSY；而服务自己的 open 就是产品路径，更有意义。

snapshot() {
    local tag="$1"

    say "4.$tag 阶段计数"
    board "sudo -n cat $TRACE/trace" 2>/dev/null > "/tmp/vin_trace_$tag.txt"

    printf '   %-20s %-6s %s\n' "阶段事件" "次数" "内核函数"
    printf '   %s\n' "----------------------------------------------------------"
    for r in "${PROBES_OK[@]}"; do
        IFS='|' read -r ev fn desc <<< "$r"
        n=$(grep -c ": ${ev}:" "/tmp/vin_trace_$tag.txt" 2>/dev/null || true)
        printf '   %-20s %-6s %s\n' "$ev" "${n:-0}" "$fn"
    done
    for r in "${PROBES_FAIL[@]}"; do
        IFS='|' read -r ev fn desc <<< "$r"
        printf '   %-20s %-6s %s\n' "$ev" "N/A" "$fn  (探针未安装)"
    done

    # 环形缓冲溢出会让计数偏小，必须显式报出来，否则会把「丢事件」读成「次数少」
    local ovf
    ovf=$(board "sudo -n sh -c 'cat $TRACE/per_cpu/cpu*/stats 2>/dev/null | awk -v k=overrun '\$1==k{s+=\$2} END{print s+0}'" 2>/dev/null | tr -d '\r')
    [ "${ovf:-0}" != "0" ] && warn "trace overrun=${ovf}，上面的计数是**下界**，不是精确值"

    say "4.$tag /proc/interrupts"
    board "grep -E 'tdm|isp|vinc' /proc/interrupts" 2>/dev/null | sed 's/^/   /'

    # /mpp/vi 是全树唯一静态确认「零 MMIO」的 VIN 调试节点（vin_core.c:637-971），
    # 但它遍历全部 18 个 vin_core，且其中六处 pipe.sd[SENSOR] 没有判空。
    # 只在「服务刚 open 过、pipe 已填充」时才读，且失败就放弃，不重试。
    say "4.$tag /sys/kernel/debug/mpp/vi（仅在服务已 open 时）"
    if board 'sudo -n test -r /sys/kernel/debug/mpp/vi' 2>/dev/null; then
        if board 'sudo -n head -c 4000 /sys/kernel/debug/mpp/vi' 2>/dev/null | sed 's/^/   /'; then
            :
        else
            warn "读 /mpp/vi 失败（可能空指针解引用），已放弃，不重试"
        fi
    else
        warn "无 /sys/kernel/debug/mpp/vi"
    fi
}

trigger_open() {
    local tag="$1"
    say "3.$tag 清 trace 并触发一次 open"

    board "sudo -n sh -c 'echo > $TRACE/trace'" >/dev/null 2>&1

    if [ "$NO_RESTART" = 1 ]; then
        warn "--no-restart：请立刻手动重启 weaknet-server，等待 ${WIN}s"
        note "  ssh $BOARD 'sudo touch $MAINT && sudo systemctl restart weaknet-server && sudo rm -f $MAINT'"
        sleep "$WIN"
        return
    fi

    # 维护标记 → 重启 → 立刻删标记 → 等观察窗口
    # 标记必须在 restart 返回后马上删：留着会让守卫长期不按健康判据工作。
    board "sudo -n touch $MAINT" >/dev/null 2>&1
    if ! board 'sudo -n systemctl restart weaknet-server' 2>/dev/null; then
        warn "restart weaknet-server 失败，继续采当前 trace"
    fi
    board "sudo -n rm -f $MAINT" >/dev/null 2>&1
    note "服务已重启，观察 ${WIN}s"
    sleep "$WIN"
}

trigger_open 1
snapshot 1

trigger_open 2
snapshot 2

# ------------------------------------------------------------ 5. 差异表
say "5. 两轮对比"
printf '   %-20s %-8s %-8s %s\n' "阶段事件" "open#1" "open#2" "判读"
printf '   %s\n' "----------------------------------------------------------------------"
for r in "${PROBES_OK[@]}"; do
    IFS='|' read -r ev fn desc <<< "$r"
    n1=$(grep -c ": ${ev}:" /tmp/vin_trace_1.txt 2>/dev/null || true)
    n2=$(grep -c ": ${ev}:" /tmp/vin_trace_2.txt 2>/dev/null || true)
    n1=${n1:-0}; n2=${n2:-0}
    verdict="—"
    if [ "$n1" = "0" ] && [ "$n2" = "0" ]; then
        verdict="两次都没到 ⇒ 这条路径被跳过了"
    elif [ "$n1" != "0" ] || [ "$n2" != "0" ]; then
        verdict="到达过"
    fi
    printf '   %-20s %-8s %-8s %s\n' "$ev" "$n1" "$n2" "$verdict"
done

say "6. 判读要点（照这个顺序读，不要跳步）"
cat <<'EOF'
   1. 先看 vin_tdm_rxen（csic_tdm_rx_enable）：
        0 次 ⇒ TDM 接收通道从未使能，这就是零中断的直接原因。
             往回看 vin_tdm_top（sunxi_tdm_top_s_stream）：
               也是 0 次 ⇒ 在它之前就返回了（查 work_mode / speed_dn_en / WDR 检查）
               非 0 次   ⇒ top 跑了但 rx_enable 没跑（查 tdm_set_rx_cfg 的返回值）
       非 0 次 ⇒ 接收通道开过，零帧原因在别处，不要再往这条线上查。

   2. 再看 vin_tdm_isr：
        0 次 且 rxen 非 0 ⇒ 通道开了但硬件确实没有产生中断
       （数据没到 / PHY 没锁 / MIPI 没出包），方向转向 sensor-MIPI-PHY。
       非 0 次           ⇒ 中断在进，看 tdm_isr 里的 vin_err 分支命中哪一条。

   3. 再看 vin_mipi_mbuscfg 与 vin_combo_csi：
        前者 0 次 ⇒ 图遍历没走到 MIPI 实体（sensor→mipi 链路没使能，
                    或 mipi 不在遍历路径上）。
        后者 0 次 ⇒ if_type 不是 CSI2_DPHY（combo_csi_init 里 UNKNOWN/CPHY
                    等分支都是空转），PHY 从未被配置。
        两者都非 0 ⇒ PHY 配置流程走过，问题在更上层（sensor 是否真的出流）。

   4. 探针 FAIL 的函数一律视为「未知」，不是「没调用」。
      尤其 static 函数可能被内联，装不上是常态。

   5. 所有计数都受两个前提约束：
        - 观察窗口只有一小段时间（默认 40s，脚本顶部 WIN），窗口外看不到；
        - trace overrun 时计数是下界。
      结论要写成「在这两个窗口内到达了 N 次」，不要写成「从未发生」。
EOF

say "完成"
note "trace 原文留在宿主机 /tmp/vin_trace_{1,2}.txt"
note "板端探针已由 trap 清理；维护标记已删除"
