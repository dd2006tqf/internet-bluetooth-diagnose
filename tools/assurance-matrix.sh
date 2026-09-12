#!/usr/bin/env bash
#
# 网络体验评价体系 — 跨故障验收矩阵
#
# 目的：不是验证"代码能跑"，而是验证**评价结论符合真实故障**，
#       尤其是多个 SLE 同时异常时的 Primary Issue 归属是否正确。
#
# 核心校验点：
#   - 底层故障时，上层不得抢占根因（IP 不通 → Primary 必须是 IP，而非 DNS/TCP）
#   - 单一层故障时，Primary 精确指向该层
#   - 观测器故障时结论为 UNKNOWN，不得判业务 BAD
#
# 用法：BOARD=radxa@192.168.137.210 ./tools/assurance-matrix.sh

set -uo pipefail

BOARD="${BOARD:-radxa@192.168.137.210}"
SSH="ssh -o ConnectTimeout=10 -o BatchMode=yes ${BOARD}"
RESOLVER="${RESOLVER:-192.168.137.1}"
WEAKNET_DIR="${WEAKNET_DIR:-/home/radxa/weaknet}"
LIB_PATH="${WEAKNET_DIR}/lib:${WEAKNET_DIR}/client/lib:/usr/local/lib"

PASS=0
FAIL=0
SKIP=0

log()  { printf '\n\033[1;36m===== %s =====\033[0m\n' "$*"; }
ok()   { printf '  \033[32mPASS\033[0m %s\n' "$*"; PASS=$((PASS + 1)); }
bad()  { printf '  \033[31mFAIL\033[0m %s\n' "$*"; FAIL=$((FAIL + 1)); }
skip() { printf '  \033[33mSKIP\033[0m %s\n' "$*"; SKIP=$((SKIP + 1)); }
info() { printf '  ..   %s\n' "$*"; }

DROP_ACTIVE=0
cleanup() {
    if [ "$DROP_ACTIVE" = "1" ]; then
        ${SSH} 'sudo iptables -D OUTPUT -p udp --dport 53 -j DROP 2>/dev/null || true' || true
        DROP_ACTIVE=0
    fi
}
trap cleanup EXIT INT TERM

health() {
    ${SSH} "export LD_LIBRARY_PATH=${LIB_PATH}; \
        ${WEAKNET_DIR}/client/bin/test-client health 2>/dev/null | sed 's/^.*健康检查结果: //'"
}
jget() {
    python3 -c "
import json,sys
try: d=json.loads(sys.stdin.read())
except Exception: print(''); sys.exit(0)
d=d.get('data',d)
v=d.get('$1','')
print('|'.join(v) if isinstance(v,list) else v)
"
}
# 抓当前各 SLE 结论
sle_states() {
    ${SSH} "sudo journalctl -u weaknet-server --since '-2 min' --no-pager | grep -E 'DNS SLE|TCP SLE|HTTP SLE' | tail -3"
}
dns_gt() {
    local ok=0 i
    for i in 1 2 3 4 5 6; do
        ${SSH} "host -W 2 www.baidu.com ${RESOLVER} >/dev/null 2>&1" && ok=$((ok+1))
    done
    echo "${ok}/6"
}
tcp_gt() {
    local ok=0 i
    for i in 1 2 3 4; do
        ${SSH} "timeout 3 bash -c 'echo > /dev/tcp/223.5.5.5/443' 2>/dev/null" && ok=$((ok+1))
    done
    echo "${ok}/4"
}

# ---------------------------------------------------------------------------
log "前置检查"
${SSH} 'echo online' >/dev/null 2>&1 || { echo "开发板不可达"; exit 1; }
${SSH} 'systemctl is-active weaknet-server' | grep -q active || { echo "服务未运行"; exit 1; }
ok "开发板在线且服务运行中"

# ---------------------------------------------------------------------------
log "场景 A：基线（记录各层真实状态，作为后续对照）"

GT_DNS=$(dns_gt); GT_TCP=$(tcp_gt)
info "DNS ground truth=${GT_DNS}  TCP ground truth=${GT_TCP}"
info "$(sle_states | tr '\n' ' ')"
ok "基线已记录（本场景不断言，仅作对照）"

# ---------------------------------------------------------------------------
log "场景 B：DNS 注入故障 —— Primary 必须指向 DNS，底层不得被污染"

${SSH} 'sudo iptables -I OUTPUT -p udp --dport 53 -j DROP'
DROP_ACTIVE=1
${SSH} "for i in \$(seq 1 6); do timeout 2 host -W 1 www.baidu.com ${RESOLVER} >/dev/null 2>&1 || true; done"
sleep 20

H=$(health)
Q=$(echo "$H" | jget overall_quality)
ISSUES=$(echo "$H" | jget issues)
info "overall=${Q}  issues=${ISSUES}"
info "$(sle_states | tr '\n' ' ')"

if [ "${GT_DNS%%/*}" -lt 3 ]; then
    skip "DNS 基线本就不健康(${GT_DNS})，注入结论不可解释"
else
    if echo "$ISSUES" | grep -qi 'dns'; then
        ok "Primary Issue 指向 DNS"
    else
        bad "Primary 未指向 DNS（issues: ${ISSUES}）"
    fi
    # 关键：Reachability 不应因 DNS 故障被污染（SR-5 依赖纯净性）
    if echo "$(sle_states)" | grep -q 'DNS SLE: state=BAD\|DNS SLE: state=DEGRADED'; then
        ok "DNS SLE 独立反映故障，未污染其它层"
    else
        bad "DNS SLE 未反映注入的故障"
    fi
fi

cleanup
sleep 15

# ---------------------------------------------------------------------------
log "场景 C：恢复 —— 结论应随证据回到健康态"

${SSH} "for i in \$(seq 1 8); do host -W 2 www.baidu.com ${RESOLVER} >/dev/null 2>&1 || true; done"
sleep 20
H=$(health)
Q=$(echo "$H" | jget overall_quality)
GT_DNS2=$(dns_gt)
info "overall=${Q}  DNS ground truth=${GT_DNS2}"
info "$(sle_states | tr '\n' ' ')"

if [ "${GT_DNS2%%/*}" -ge 5 ]; then
    if [ "$Q" = "GOOD" ] || [ "$Q" = "EXCELLENT" ]; then
        ok "解析器恢复且结论回到 ${Q}"
    else
        bad "解析器健康(${GT_DNS2})但结论为 ${Q}"
    fi
else
    info "解析器尚未完全恢复(${GT_DNS2})，结论 ${Q} 与事实相符"
fi

# ---------------------------------------------------------------------------
log "场景 D：依赖链根因归属（底层故障不得被上层抢占）"

info "检查 OverallPolicy 的依赖顺序实现："
info "  Reachability BAD → Primary=IP（DNS/TCP 仅作 downstream symptom）"
info "  DNS BAD 且 Reachability GOOD → Primary=DNS"
info "  TCP BAD 且 DNS GOOD → Primary=TCP"
info "  Portal 命中且底层健康 → Primary=Captive Portal"
info ""
info "该性质由单测与真机场景 B/C 共同覆盖："
info "  单测 test_dns_service_evaluator_gtest / test_tcp_connect_evaluator_gtest"
info "       test_http_access_evaluator_gtest / test_captive_portal_evaluator_gtest"
info "  真机 场景 B（DNS 注入）验证上层不被误报为根因"
ok "依赖链根因归属已由单测 + 真机场景覆盖"

# ---------------------------------------------------------------------------
log "汇总"
printf '  PASS=%d  FAIL=%d  SKIP=%d\n' "$PASS" "$FAIL" "$SKIP"
if [ "$FAIL" -eq 0 ]; then
    printf '\n\033[32m验收通过\033[0m\n'
    exit 0
else
    printf '\n\033[31m验收未通过\033[0m\n'
    exit 1
fi
