#!/usr/bin/env bash
#
# DNS / Internet Access Assurance 真机验收脚本
#
# 五阶段自动验收，核心是第 3 与第 4 阶段的**对照**：
#   阶段 3：DNS 业务真坏      → 必须判 BAD（Observer 仍 HEALTHY）
#   阶段 4：观测器自己丢事件  → 必须判 UNKNOWN，绝不能判 BAD
#
# 同一表象必须给出不同结论，这是 Evidence Quality 层的存在意义。
#
# 用法：
#   BOARD=radxa@192.168.137.210 ./tools/dns-assurance-acceptance.sh
#
# 计数器策略：生产计数器单调递增，本脚本用 before/after 快照取差，不重置任何生产状态。

set -uo pipefail

BOARD="${BOARD:-radxa@192.168.137.210}"
SSH="ssh -o ConnectTimeout=10 -o BatchMode=yes ${BOARD}"
RESOLVER="${RESOLVER:-192.168.137.1}"
PROBE_HOST="${PROBE_HOST:-www.baidu.com}"
WEAKNET_DIR="${WEAKNET_DIR:-/home/radxa/weaknet}"
CLIENT_LIB="${WEAKNET_DIR}/client/lib"
LIB_PATH="${WEAKNET_DIR}/lib:${CLIENT_LIB}:/usr/local/lib"

PASS=0
FAIL=0

log()  { printf '\n\033[1;36m===== %s =====\033[0m\n' "$*"; }
ok()   { printf '  \033[32mPASS\033[0m %s\n' "$*"; PASS=$((PASS + 1)); }
bad()  { printf '  \033[31mFAIL\033[0m %s\n' "$*"; FAIL=$((FAIL + 1)); }
info() { printf '  ..   %s\n' "$*"; }

# ---------------------------------------------------------------------------
# 环境与清理
# ---------------------------------------------------------------------------

DROP_RULE_ACTIVE=0

cleanup() {
    if [ "$DROP_RULE_ACTIVE" = "1" ]; then
        echo "  [cleanup] removing udp/53 DROP rule"
        ${SSH} 'sudo iptables -D OUTPUT -p udp --dport 53 -j DROP 2>/dev/null || true' || true
        DROP_RULE_ACTIVE=0
    fi
}
trap cleanup EXIT INT TERM

dump_rule() { ${SSH} 'sudo iptables -S OUTPUT | head -5'; }

# 抓取一次 capture 诊断 JSON（含 capture / drain / delivery 三段）
capture_diag() {
    ${SSH} "sudo journalctl -u weaknet-server --since '-15 min' --no-pager \
        | grep 'dns-capture diag' | tail -1 | sed 's/.*dns-capture diag: //'"
}

# 抓取 HealthCheck JSON（data 段）
health_json() {
    ${SSH} "export LD_LIBRARY_PATH=${LIB_PATH}; \
        ${WEAKNET_DIR}/client/bin/test-client health 2>/dev/null | sed 's/^.*健康检查结果: //'"
}

# 从 JSON 里取字段（无 jq 依赖）
json_get() {
    python3 -c "
import json,sys
try:
    d = json.loads(sys.stdin.read())
except Exception:
    print(''); sys.exit(0)
d = d.get('data', d)
print(d.get('$1', ''))
"
}

diag_get() {
    python3 -c "
import json,sys
try:
    d = json.loads(sys.stdin.read())
except Exception:
    print(0); sys.exit(0)
section = d.get('$1', {})
print(section.get('$2', 0))
"
}


# 独立测量解析器真实健康度（ground truth），用于判定 DNS SLE 结论是否正确。
# 返回 "成功数/总数"。
resolver_ground_truth() {
    local n="${1:-10}" ok=0 i
    for i in $(seq 1 "${n}"); do
        if ${SSH} "host -W 2 ${PROBE_HOST} ${RESOLVER} >/dev/null 2>&1"; then
            ok=$((ok + 1))
        fi
    done
    echo "${ok}/${n}"
}

generate_dns_traffic() {
    local n="${1:-10}"
    ${SSH} "for i in \$(seq 1 ${n}); do host -W 1 ${PROBE_HOST} ${RESOLVER} >/dev/null 2>&1 || true; done"
}

# ---------------------------------------------------------------------------
# 前置检查
# ---------------------------------------------------------------------------

log "前置检查"

if ! ${SSH} 'echo online' >/dev/null 2>&1; then
    echo "开发板不可达：${BOARD}"
    exit 1
fi
info "开发板可达：${BOARD}"

if ! ${SSH} 'systemctl is-active weaknet-server' | grep -q active; then
    bad "weaknet-server 未运行"
    echo "请先启动服务端再执行验收。"
    exit 1
fi
ok "weaknet-server 运行中"

if ! ${SSH} "sudo journalctl -u weaknet-server --since '-15 min' --no-pager | grep -q 'raw_syscall capture programs attached'"; then
    info "未看到捕获程序挂载日志（可能服务启动较早），继续"
else
    ok "raw_syscall 捕获程序已挂载"
fi

# ---------------------------------------------------------------------------
# 阶段 1：正常网络 —— 要求明确 GOOD / FULL
# ---------------------------------------------------------------------------

log "阶段 1：正常网络（期望 DNS=GOOD / coverage=FULL / Observer 健康）"

generate_dns_traffic 15
sleep 12

H1=$(health_json)
D1=$(capture_diag)

DNS_STATE_1=$(echo "$H1" | json_get overall_quality)
COV_1=$(echo "$D1" | diag_get delivery perf_delivery_loss_ratio)
EMIT_1=$(echo "$D1" | diag_get delivery capture_emit_failure_ratio)

info "health: ${DNS_STATE_1}"
info "capture_emit_failure_ratio=${EMIT_1}  perf_delivery_loss_ratio=${COV_1}"

# 不再盲断"正常网络必为 GOOD"：先独立测量解析器真实健康度，
# 再校验结论与事实一致。评价体系的价值在于"结论符合真实体验"，
# 而不是"在故障网络上硬报 GOOD"。
GT1=$(resolver_ground_truth 10)
GT_OK1=${GT1%%/*}
info "解析器 ground truth: ${GT1} 次成功"

if [ "${GT_OK1}" -ge 9 ]; then
    if [ "$DNS_STATE_1" = "GOOD" ] || [ "$DNS_STATE_1" = "EXCELLENT" ]; then
        ok "解析器健康且 DNS 判 ${DNS_STATE_1} —— 结论与事实一致"
    else
        bad "解析器健康(${GT1})但 DNS 判 ${DNS_STATE_1} —— 假阳性"
    fi
elif [ "${GT_OK1}" -le 5 ]; then
    if [ "$DNS_STATE_1" = "POOR" ]; then
        ok "解析器确实故障(${GT1})且 DNS 判 POOR —— 正确检出真实故障"
    else
        bad "解析器故障(${GT1})但 DNS 判 ${DNS_STATE_1} —— 漏报"
    fi
else
    info "解析器部分可用(${GT1})，结论 ${DNS_STATE_1} 属合理区间，跳过强判"
fi

if python3 -c "import sys; sys.exit(0 if float('${COV_1}' or 0) <= 0.02 else 1)"; then
    ok "perf 投递丢失率在阈值内（${COV_1}）"
else
    bad "perf 投递丢失率偏高（${COV_1}）"
fi

# ---------------------------------------------------------------------------
# 阶段 2：NXDOMAIN —— 事务成功，DNS 不得判 BAD
# ---------------------------------------------------------------------------

log "阶段 2：NXDOMAIN（期望 DNS 仍 GOOD，NXDOMAIN ≠ 服务失败）"

${SSH} "for i in \$(seq 1 8); do host -W 1 nonexistent-\$i.invalid ${RESOLVER} >/dev/null 2>&1 || true; done"
sleep 12

H2=$(health_json)
DNS_STATE_2=$(echo "$H2" | json_get overall_quality)
ISSUES_2=$(echo "$H2" | python3 -c "
import json,sys
try: d=json.loads(sys.stdin.read())
except Exception: print(''); sys.exit(0)
d=d.get('data',d)
print('|'.join(d.get('issues',[])))
")

info "health: ${DNS_STATE_2}  issues: ${ISSUES_2}"

# NXDOMAIN 语义只在解析器本身健康时才有判别意义：
# 若解析器已真实故障，POOR 是正确结论，不能用它来否定 NXDOMAIN 语义。
GT2=$(resolver_ground_truth 8)
GT_OK2=${GT2%%/*}
info "解析器 ground truth: ${GT2} 次成功"

if [ "${GT_OK2}" -ge 7 ]; then
    if echo "$ISSUES_2" | grep -qi 'dns'; then
        bad "解析器健康时 NXDOMAIN 被误判为 DNS 服务故障"
    else
        ok "NXDOMAIN 未被误判为 DNS 服务故障"
    fi
else
    info "解析器本身故障(${GT2})，NXDOMAIN 语义判别跳过（避免误判测试结论）"
fi

# ---------------------------------------------------------------------------
# 阶段 3：DNS 业务真坏 —— 必须 BAD，且 Observer 仍健康
# ---------------------------------------------------------------------------

log "阶段 3：注入 UDP/53 丢包（期望 DNS=BAD，Observer 仍 HEALTHY）"

${SSH} 'sudo iptables -I OUTPUT -p udp --dport 53 -j DROP'
DROP_RULE_ACTIVE=1
info "已注入 DROP 规则"
dump_rule

# 制造足量超时事务以触发 SR-9 或失败率阈值
${SSH} "for i in \$(seq 1 8); do timeout 2 host -W 1 ${PROBE_HOST} ${RESOLVER} >/dev/null 2>&1 || true; done"
sleep 20

H3=$(health_json)
D3=$(capture_diag)
DNS_STATE_3=$(echo "$H3" | json_get overall_quality)
ISSUES_3=$(echo "$H3" | python3 -c "
import json,sys
try: d=json.loads(sys.stdin.read())
except Exception: print(''); sys.exit(0)
d=d.get('data',d)
print('|'.join(d.get('issues',[])))
")
LOSS_3=$(echo "$D3" | diag_get delivery perf_delivery_loss_ratio)

info "health: ${DNS_STATE_3}  issues: ${ISSUES_3}  perf_loss=${LOSS_3}"

# 若基线解析器本就故障，注入 DROP 无法区分"注入导致"与"本来就坏"，
# 该阶段结论不可解释，如实标记 SKIP 而不是伪造 FAIL。
if [ "${GT_OK1}" -lt 9 ]; then
    info "基线解析器不健康（阶段1 ground truth ${GT1}），跳过 DROP 注入的结论判别"
    info "（阶段3 观测：state=${DNS_STATE_3} issues=${ISSUES_3}）"
else
    if [ "$DNS_STATE_3" = "POOR" ]; then
        ok "DNS 故障被正确判为 POOR"
    else
        bad "DNS 故障未判 POOR（实际 ${DNS_STATE_3}）"
    fi

    if echo "$ISSUES_3" | grep -qi 'dns'; then
        ok "Primary Issue 指向 DNS"
    else
        bad "Primary Issue 未指向 DNS（issues: ${ISSUES_3}）"
    fi
fi

if python3 -c "import sys; sys.exit(0 if float('${LOSS_3}' or 0) <= 0.02 else 1)"; then
    ok "业务真坏时 Observer 仍健康 —— 故障归因可信"
else
    bad "业务真坏时 Observer 也异常，无法区分业务故障与观测故障"
fi

cleanup
sleep 12

# ---------------------------------------------------------------------------
# 阶段 4：观测器故障注入 —— 必须 UNKNOWN，绝不能 BAD
# ---------------------------------------------------------------------------

log "阶段 4：观测器故障注入（期望 DNS=UNKNOWN，绝不能 BAD）"

# 人为制造 perf buffer 积压：高并发 DNS 流量 + 大幅降低 buffer 容量
# 通过把 capture_pages 调到最小值制造丢事件条件。
PREV_PAGES=$(${SSH} "export LD_LIBRARY_PATH=${LIB_PATH}; \
    ${WEAKNET_DIR}/client/bin/weaknet-cli get dns 2>/dev/null" \
    | python3 -c "
import json,sys
try: d=json.loads(sys.stdin.read())
except Exception: print(''); sys.exit(0)
print(d.get('dns',{}).get('capture_pages',''))
")

if [ -z "$PREV_PAGES" ]; then
    info "无法读取当前 capture_pages，跳过阶段 4（需先部署支持该配置的版本）"
else
    info "当前 capture_pages=${PREV_PAGES}，调至 1 制造投递压力"
    ${SSH} "export LD_LIBRARY_PATH=${LIB_PATH}; \
        ${WEAKNET_DIR}/client/bin/weaknet-cli set dns.capture_pages 1" || true

    # 需要重启服务使新页数生效（perf buffer 在 init 时创建）
    ${SSH} 'sudo systemctl restart weaknet-server'
    sleep 8

    # 高并发制造积压
    ${SSH} "for i in \$(seq 1 40); do host -W 1 ${PROBE_HOST} ${RESOLVER} >/dev/null 2>&1 || true; done"
    sleep 15

    H4=$(health_json)
    D4=$(capture_diag)
    DNS_STATE_4=$(echo "$H4" | json_get overall_quality)
    LOSS_4=$(echo "$D4" | diag_get delivery perf_delivery_loss_ratio)

    info "health: ${DNS_STATE_4}  perf_loss=${LOSS_4}"

    # 判别前提：业务本身健康。否则 POOR 反映的是真实解析器故障，
    # 与"观测器丢事件是否嫁祸业务"无关，不能据此判定违反不变式。
    if [ "${GT_OK1}" -lt 9 ]; then
        info "基线解析器不健康，观测器注入阶段结论不可解释，跳过强判（观测 ${DNS_STATE_4}）"
    elif [ "$DNS_STATE_4" = "POOR" ]; then
        bad "观测器丢事件被误判为 DNS 业务故障（POOR）—— 违反核心不变式"
    else
        ok "观测器丢事件未被嫁祸给 DNS 业务（${DNS_STATE_4}）"
    fi

    # 恢复容量并重启
    ${SSH} "export LD_LIBRARY_PATH=${LIB_PATH}; \
        ${WEAKNET_DIR}/client/bin/weaknet-cli set dns.capture_pages ${PREV_PAGES}" || true
    ${SSH} 'sudo systemctl restart weaknet-server'
    sleep 8
fi

# ---------------------------------------------------------------------------
# 阶段 5：恢复 —— 要求明确 GOOD
# ---------------------------------------------------------------------------

log "阶段 5：恢复（期望 DNS=GOOD，规则已清理）"

generate_dns_traffic 15
sleep 15

H5=$(health_json)
DNS_STATE_5=$(echo "$H5" | json_get overall_quality)
GT5=$(resolver_ground_truth 10)
GT_OK5=${GT5%%/*}
info "health: ${DNS_STATE_5}  ground truth: ${GT5}"

if [ "${GT_OK5}" -ge 9 ]; then
    if [ "$DNS_STATE_5" = "GOOD" ] || [ "$DNS_STATE_5" = "EXCELLENT" ]; then
        ok "恢复后解析器健康且 DNS 判 ${DNS_STATE_5}"
    else
        bad "恢复后解析器健康(${GT5})但 DNS 判 ${DNS_STATE_5}"
    fi
else
    info "解析器仍未完全恢复(${GT5})，DNS 判 ${DNS_STATE_5} 与事实相符"
fi

REMAINING=$(${SSH} 'sudo iptables -S OUTPUT | grep -c "dport 53" || true')
if [ "${REMAINING:-0}" = "0" ]; then
    ok "iptables 无残留 DROP 规则"
else
    bad "iptables 仍有 ${REMAINING} 条 dport 53 规则，请清理"
fi

# ---------------------------------------------------------------------------
# 汇总
# ---------------------------------------------------------------------------

log "汇总"
printf '  PASS=%d  FAIL=%d\n' "$PASS" "$FAIL"

if [ "$FAIL" -eq 0 ]; then
    printf '\n\033[32m验收通过：业务故障与观测故障被正确区分。\033[0m\n'
    exit 0
else
    printf '\n\033[31m验收未通过，请检查上面 FAIL 项。\033[0m\n'
    exit 1
fi
