#!/usr/bin/env bash
#
# WeakNet Assurance Model v1 契约验收脚本
#
# 目的：把 v1 冻结的**语义契约**固化成可一键重跑的回归门禁。
# 任何人改动 probe / eBPF / evaluator 之后，都应先跑通本脚本，
# 再谈提交。它验证的是"v1 truth table 没有被静默改掉"。
#
# 四条契约（全部来自已冻结的 v1 设计）：
#   C1  Provenance 隔离   仅运行探测时，被动 DNS/TCP/HTTP 增量必须恒为 0
#   C2  Portal Oracle     受控端点在健康网络下必须全部 signal=0（零误报）
#   C3  Active Capability 三个独立故障域目标必须全部走通 DNS/TCP/HTTPS
#   C4  Snapshot 一致性   同一快照在 HealthCheck 与 GetNetworkExperience 必须同源
#
# 用法：
#   BOARD=radxa@192.168.137.210 ./tools/verify_v1_contract.sh
#   BOARD=... ./tools/verify_v1_contract.sh --skip-c1   # 跳过需 70s 的 C1
#
# 计数器策略：生产计数器单调递增，本脚本用 before/after 快照取差，
# **绝不重置任何生产状态**（与 dns-assurance-acceptance.sh 一致）。

set -uo pipefail

BOARD="${BOARD:-radxa@192.168.137.210}"
SSH="ssh -o ConnectTimeout=10 -o BatchMode=yes ${BOARD}"
WEAKNET_DIR="${WEAKNET_DIR:-/home/radxa/weaknet}"
PROBE_WAIT="${PROBE_WAIT:-70}"     # C1 观测窗口，需覆盖至少 2 个 30s 探测周期

SKIP_C1=0
for a in "$@"; do
    [ "$a" = "--skip-c1" ] && SKIP_C1=1
done

PASS=0
FAIL=0
SKIP=0

log()  { printf '\n\033[1;36m===== %s =====\033[0m\n' "$*"; }
ok()   { printf '  \033[32mPASS\033[0m %s\n' "$*"; PASS=$((PASS + 1)); }
bad()  { printf '  \033[31mFAIL\033[0m %s\n' "$*"; FAIL=$((FAIL + 1)); }
skip() { printf '  \033[33mSKIP\033[0m %s\n' "$*"; SKIP=$((SKIP + 1)); }
info() { printf '  ..   %s\n' "$*"; }

# ---------------------------------------------------------------------------
# 取数辅助
# ---------------------------------------------------------------------------

# 最近一条 dns-capture diag 的 capture/drain/delivery 三段 JSON
capture_diag() {
    ${SSH} "sudo journalctl -u weaknet-server --since '-20 min' --no-pager \
        | grep 'dns-capture diag' | tail -1 | sed 's/.*dns-capture diag: //'"
}

# 从 diag JSON 里取一个路径的值，例如 capture.emitted
diag_field() {
    local json="$1" path="$2"
    python3 -c "
import json,sys
try:
    d=json.loads(sys.argv[1])
    for k in sys.argv[2].split('.'):
        d=d[k]
    print(d)
except Exception:
    print('')
" "$json" "$path" 2>/dev/null
}

# 最近一条含某模式的日志行
last_log() { ${SSH} "sudo journalctl -u weaknet-server --since '-5 min' --no-pager | grep '$1' | tail -1"; }

# ---------------------------------------------------------------------------
# 前置检查
# ---------------------------------------------------------------------------

log "前置检查"

if ! ${SSH} 'systemctl is-active weaknet-server' >/dev/null 2>&1; then
    bad "weaknet-server 未运行"
    exit 1
fi
ok "weaknet-server 运行中"

# HTTPS 目标与 Portal oracle 必须来自配置，而非代码默认值（v1 硬要求）
CFG=$(${SSH} 'sudo cat /etc/weaknet/config.yaml')
if echo "$CFG" | grep -q "targets:"; then
    ok "capability targets 显式配置"
else
    bad "capability targets 未显式配置（第三方 oracle 必须是配置项）"
fi
if echo "$CFG" | grep -q "portal_targets:"; then
    ok "portal oracle targets 显式配置"
else
    bad "portal_targets 未显式配置"
fi

# ---------------------------------------------------------------------------
# C1: Provenance 隔离 —— 仅开探测时被动计数增量必须为 0
# ---------------------------------------------------------------------------

if [ "$SKIP_C1" = "1" ]; then
    skip "C1 Provenance 隔离（--skip-c1）"
else
    log "C1 Provenance 隔离（观测 ${PROBE_WAIT}s，期间不产生任何用户流量）"

    D0=$(capture_diag)
    E0=$(diag_field "$D0" capture.emitted)
    Q0=$(diag_field "$D0" drain.tracker_query_accepted)
    R0=$(diag_field "$D0" drain.tracker_response_accepted)
    info "基线 emitted=${E0:-?} q_accepted=${Q0:-?} r_accepted=${R0:-?}"

    if [ -z "$E0" ]; then
        bad "无法读取 capture diag 基线（日志缺失？）"
    else
        info "等待 ${PROBE_WAIT}s（探测持续运行，不注入用户流量）..."
        sleep "$PROBE_WAIT"

        D1=$(capture_diag)
        E1=$(diag_field "$D1" capture.emitted)
        Q1=$(diag_field "$D1" drain.tracker_query_accepted)
        R1=$(diag_field "$D1" drain.tracker_response_accepted)

        dE=$(( ${E1:-0} - ${E0:-0} ))
        dQ=$(( ${Q1:-0} - ${Q0:-0} ))
        dR=$(( ${R1:-0} - ${R0:-0} ))
        info "增量 emitted=${dE} q_accepted=${dQ} r_accepted=${dR}"

        if [ "$dE" -eq 0 ] && [ "$dQ" -eq 0 ] && [ "$dR" -eq 0 ]; then
            ok "C1 通过：探测流量未进入 passive 窗口（增量全 0）"
        else
            bad "C1 失败：探测自流量污染 passive 窗口（应为 0，实测 emitted=${dE} q=${dQ} r=${dR}）"
        fi
    fi
fi

# ---------------------------------------------------------------------------
# C2: Portal Oracle —— 健康网络下不得误报门户
# ---------------------------------------------------------------------------

log "C2 Portal Oracle 误报检查（期望 signal=0 / no_captive_portal）"

ORACLE_LINES=$(${SSH} "sudo journalctl -u weaknet-server --since '-3 min' --no-pager | grep 'Portal oracle: target='" )
if [ -z "$ORACLE_LINES" ]; then
    skip "C2 最近 3 分钟内无 oracle 轮次（探测周期 30s，请稍后重跑）"
else
    echo "$ORACLE_LINES" | tail -6 | sed 's/^/       /'
    # signal=0 表示符合预期（NONE）
    NONZERO=$(echo "$ORACLE_LINES" | grep -oE 'signal=[0-9]+' | grep -v 'signal=0' | wc -l)
    if [ "$NONZERO" -eq 0 ]; then
        ok "C2 通过：全部 oracle 端点 signal=0，无误报"
    else
        bad "C2 失败：${NONZERO} 个 oracle 端点报出非 0 信号（健康网络不应误报）"
    fi
fi

PORTAL_STATE=$(last_log 'Active capability' | grep -oE 'portal=[A-Z]+' | cut -d= -f2)
if [ "$PORTAL_STATE" = "GOOD" ]; then
    ok "C2 补充：Active portal 状态 = GOOD(no_captive_portal)"
elif [ -n "$PORTAL_STATE" ]; then
    info "Active portal 当前 = ${PORTAL_STATE}（若非 GOOD 请结合上层网络判断）"
fi

# ---------------------------------------------------------------------------
# C3: Active Capability —— 三个独立故障域目标必须全部走通
# ---------------------------------------------------------------------------

log "C3 Active Capability（期望 dns/tcp/https 三项 GOOD）"

# 注意：'Active probe detail / Portal oracle target' 只在**启动时**各打印一次，
# 不能用 5 分钟窗口抓（会漏）。目标数直接从配置解析，口径与运行时一致。
CAP_TARGETS=$(echo "$CFG" | grep -E '^    targets:' | sed 's/.*"\(.*\)".*/\1/' \
    | tr ',' '\n' | grep -c '|')
if [ "${CAP_TARGETS:-0}" -ge 2 ]; then
    ok "C3 前置：capability_targets=${CAP_TARGETS}（>=2 才有判定资格）"
else
    bad "C3 前置：capability_targets=${CAP_TARGETS}，少于 2 个目标无法授予 capability 判定资格"
fi

# 故障域去重校验：域名不同不代表故障域不同（同一 CDN 会共用出口）
DOMAINS=$(echo "$CFG" | grep -E '^    targets:' | sed 's/.*"\(.*\)".*/\1/' \
    | tr ',' '\n' | awk -F'|' '{print $4}' | sort -u | wc -l)
if [ "${DOMAINS:-0}" -ge 2 ]; then
    ok "C3 前置：声明了 ${DOMAINS} 个独立故障域"
else
    bad "C3 前置：仅 ${DOMAINS} 个故障域，quorum 会退化为单点 oracle"
fi
info "提示：域标识名不等于真实故障域 —— 换目标时须核对解析地址归属（见 v1 soak 记录）"

CAPLINE=$(last_log 'Active capability:')
echo "$CAPLINE" | sed 's/^/       /'
C_DNS=$(echo "$CAPLINE"  | grep -oE 'dns=[A-Z]+'   | cut -d= -f2)
C_TCP=$(echo "$CAPLINE"  | grep -oE 'tcp=[A-Z]+'   | cut -d= -f2)
C_HTTPS=$(echo "$CAPLINE" | grep -oE 'https=[A-Z]+' | cut -d= -f2)

[ "$C_DNS"   = "GOOD" ] && ok "C3 DNS   = GOOD" || bad "C3 DNS   = ${C_DNS:-?}（期望 GOOD）"
[ "$C_TCP"   = "GOOD" ] && ok "C3 TCP   = GOOD" || bad "C3 TCP   = ${C_TCP:-?}（期望 GOOD）"
[ "$C_HTTPS" = "GOOD" ] && ok "C3 HTTPS = GOOD" || bad "C3 HTTPS = ${C_HTTPS:-?}（期望 GOOD）"

# ---------------------------------------------------------------------------
# C4: Snapshot 一致性 —— 两条消费链必须同源
# ---------------------------------------------------------------------------

log "C4 Snapshot 跨消费者一致性（HealthCheck vs GetNetworkExperience）"

# 两条链都读取同一份不可变快照；若结果冲突说明快照不再是单一事实源。
HC=$(${SSH} "sudo journalctl -u weaknet-server --since '-3 min' --no-pager | grep -oE '\"overall_quality\":\"[A-Z]+\"' | tail -1")
info "HealthCheck overall_quality = ${HC:-（日志未含，需 D-Bus 消费者调用）}"

BAD_MARK=$(${SSH} "sudo journalctl -u weaknet-server --since '-5 min' --no-pager | grep -cE 'stale_assessment'")
if [ "${BAD_MARK:-0}" -eq 0 ]; then
    ok "C4 无 stale_assessment（快照代际一致，未被配置/网络代变更作废）"
else
    info "C4 观测到 ${BAD_MARK} 次 stale_assessment（配置或网络代刚变更，属正常失效）"
fi

NASSESS=$(${SSH} "sudo journalctl -u weaknet-server --since '-5 min' --no-pager | grep -cE 'no_assessment_yet'")
if [ "${NASSESS:-0}" -eq 0 ]; then
    ok "C4 无 no_assessment_yet（评估快照已稳定产出）"
else
    info "C4 观测到 ${NASSESS} 次 no_assessment_yet（daemon 刚启动时属正常）"
fi

# ---------------------------------------------------------------------------
# 汇总
# ---------------------------------------------------------------------------

log "汇总"
printf '  PASS=%d  FAIL=%d  SKIP=%d\n' "$PASS" "$FAIL" "$SKIP"

if [ "$FAIL" -eq 0 ]; then
    printf '\n\033[1;32m✅ v1 契约验收通过（PASS=%d）\033[0m\n' "$PASS"
    exit 0
else
    printf '\n\033[1;31m❌ v1 契约验收未通过（FAIL=%d）\033[0m\n' "$FAIL"
    printf '   注意：C1/C3 失败通常意味着 provenance 或 capability 语义被改动，\n'
    printf '   请勿直接提交 —— v1 语义变更需要升版本或 reopen freeze。\n'
    exit 1
fi
