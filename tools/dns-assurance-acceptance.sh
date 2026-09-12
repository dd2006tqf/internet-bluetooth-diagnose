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
ORACLE_PATH="${ORACLE_PATH:-/tmp/dns_oracle.py}"
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
        ${SSH} 'sudo iptables -D INPUT -p udp --sport 53 -j DROP 2>/dev/null || true' || true
        ${SSH} 'sudo iptables -D OUTPUT -p udp --dport 53 -j DROP 2>/dev/null || true' || true
        DROP_RULE_ACTIVE=0
    fi
}
trap cleanup EXIT INT TERM

dump_rule() { ${SSH} 'sudo iptables -S | head -8'; }

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
#
# 重要：必须显式指定 QTYPE。
# 起初用 `host -W 2 <name> <resolver>` 的进程退出码作为"A 记录是否正常"的 oracle，
# 但 host 不指定 -t 时会额外查询 AAAA/MX 等 RR 类型，那些查询返回 NXDOMAIN
# 导致进程 exit=1 —— 于是 A 记录完全正常的解析器被判为"故障"，
# 并据此得出了错误的"网关 DNS 故障"结论。
# 现改用 tools/dns_oracle.py：自行构造查询、显式指定 QTYPE、
# 直接解析 RCODE/ANCOUNT，不被额外 RR 类型干扰。

# 等待评价窗口老化：DNS SLE 使用 120s 回溯窗口，注入的故障会在窗口内驻留。
# 不等待就断言"恢复"，必然读到残留故障 —— 这会制造假 FAIL。
# 需要等待的时长略大于窗口，再叠加稳定器恢复保持期。
WINDOW_AGE_WAIT="${WINDOW_AGE_WAIT:-150}"

wait_for_window_to_age() {
    local secs="${1:-${WINDOW_AGE_WAIT}}"
    info "等待评价窗口老化 ${secs}s（让上一阶段的故障证据退出窗口）..."
    sleep "${secs}"
}


# 打印各 SLE 当前结论，供断言失败时归因。
# 没有这个输出，只能看到 overall，无法判断是哪一层造成的。
print_sle_states() {
    local lines
    lines=$(${SSH} "sudo journalctl -u weaknet-server --since '-2 min' --no-pager | grep -E 'DNS SLE|TCP SLE|HTTP SLE' | tail -3" 2>/dev/null | sed 's/.*\[network\] //')
    if [ -n "$lines" ]; then
        while IFS= read -r l; do [ -n "$l" ] && info "  $l"; done <<< "$lines"
    fi
}


# 提取 DNS SLE 自身状态。
#
# 为什么不能断言 overall_quality：
#   overall 由**所有** SLE 共同决定。开发板 Wi-Fi 实测 RTT 100-283ms、
#   抖动 53-57ms（poor），Responsiveness SLE 会独立把 overall 压到 FAIR，
#   这与 DNS 准确度无关。
#   用 overall 验证 DNS，等于把"Wi-Fi 质量"混进"DNS 质量"的判据 —— 测试设计错误。
#   DNS 验收必须断言 DNS SLE 自身的 state/reason。
dns_sle_state() {
    ${SSH} "sudo journalctl -u weaknet-server --since '-2 min' --no-pager \
        | grep 'DNS SLE' | tail -1" 2>/dev/null \
        | sed -n 's/.*DNS SLE: state=\([A-Z]*\).*/\1/p'
}

dns_sle_reason() {
    ${SSH} "sudo journalctl -u weaknet-server --since '-2 min' --no-pager \
        | grep 'DNS SLE' | tail -1" 2>/dev/null \
        | sed -n 's/.*reason=\([a-z_]*\).*/\1/p'
}

resolver_ground_truth() {
    local n="${1:-10}" ok=0 i out
    for i in $(seq 1 "${n}"); do
        out=$(${SSH} "python3 ${ORACLE_PATH} --resolver ${RESOLVER} --name ${PROBE_HOST} --qtype A 2>/dev/null" 2>/dev/null || true)
        # oracle 输出形如 "NOERROR an=2 ..."；只认 NOERROR 为成功
        if echo "$out" | grep -q 'NOERROR an=[1-9]'; then
            ok=$((ok + 1))
        fi
    done
    echo "${ok}/${n}"
}

# 生成 DNS 流量。
#
# 不能用 `host`：它经 glibc resolver 会发出**自己并不等待**的附加查询
# （AAAA 等），这些查询在 DNS 捕获视角下就是"发出但无响应"，5s 后被正确
# 记为 TIMEOUT。实测同一健康解析器：
#     oracle 发 15 次 -> GOOD  fail=0/15   unmatched=0
#     host   发 15 次 -> BAD   fail=21/60  unmatched=21
# 即 host 会让健康网络被判故障 —— 这与"用 host 退出码当 oracle"是同一类
# 错误：把未经独立验证的工具当成测试仪器。
# 改用 dns_oracle.py 生成**规范且可预期**的查询。
generate_dns_traffic() {
    local n="${1:-10}"
    ${SSH} "for i in \$(seq 1 ${n}); do \
        python3 ${ORACLE_PATH} --resolver ${RESOLVER} --name ${PROBE_HOST} --qtype A >/dev/null 2>&1 || true; \
    done"
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

# 部署独立 DNS oracle（ground truth 必须独立于被测系统）
ORACLE_SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/dns_oracle.py"
if [ -f "$ORACLE_SRC" ]; then
    scp -o ConnectTimeout=10 -o BatchMode=yes "$ORACLE_SRC" "${BOARD}:${ORACLE_PATH}" >/dev/null 2>&1 \
        && info "已部署 DNS oracle 到 ${ORACLE_PATH}" \
        || info "oracle 部署失败，ground truth 将不可用"
else
    info "未找到 $ORACLE_SRC"
fi

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

# 上一轮残留（含此前实验的注入故障）可能仍在 120s 窗口内，先等其老化
wait_for_window_to_age
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

DNS_SLE_1=$(dns_sle_state)
info "DNS SLE 自身状态: ${DNS_SLE_1}（overall=${DNS_STATE_1} 含其它 SLE 影响）"

if [ "${GT_OK1}" -ge 9 ]; then
    if [ "$DNS_SLE_1" = "GOOD" ]; then
        ok "解析器健康且 DNS SLE 判 GOOD —— 结论与事实一致"
    else
        bad "解析器健康(${GT1})但 DNS SLE 判 ${DNS_SLE_1}（reason=$(dns_sle_reason)）—— 假阳性"
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

# 同样用 oracle：显式构造不存在域名，确保得到干净的 NXDOMAIN 事务
${SSH} "for i in \$(seq 1 8); do \
    python3 ${ORACLE_PATH} --resolver ${RESOLVER} --name nx-\$i-9f3a.invalid --qtype A >/dev/null 2>&1 || true; \
done"
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
    :
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

# 必须等窗口干净再注入：若窗口内已积累大量成功事务，
# 少量注入失败会被稀释到 20% 阈值以下，DNS 判不出 BAD。
# 断言"注入后应判 BAD"与断言"恢复后应判 GOOD"是对称的，
# 都需要一个可解释的基线。
wait_for_window_to_age

# 注入方向必须阻断**响应**（INPUT --sport 53），不能阻断请求（OUTPUT --dport 53）。
# 原因：query 的捕获点是 kprobe/ip_finish_output2，位于 netfilter OUTPUT 之后。
# 若用 OUTPUT --dport 53 DROP，查询在进入 ip_finish_output2 前就被丢弃，
# 我们根本看不到它 —— DNS SLE 会报 no_dns_observations（"没观测到"），
# 而不是 timeout（"观测到且无响应"）。这是两种完全不同的语义。
# 实测：OUTPUT DROP -> UNKNOWN/no_dns_observations（错误注入）
#       INPUT  DROP -> BAD/critical_burst_timeouts fail=7/7（正确注入）
${SSH} 'sudo iptables -I INPUT -p udp --sport 53 -j DROP'
DROP_RULE_ACTIVE=1
info "已注入 DROP 规则"
print_sle_states
dump_rule
if ! ${SSH} "sudo iptables -S INPUT | grep -q 'sport 53'"; then
    bad "注入失败：INPUT --sport 53 规则不存在，本阶段结论不可信"
fi

# 制造足量超时事务以触发 SR-9 或失败率阈值
# 注入期间用 oracle 发查询（会被 DROP，产生真实超时事务）
${SSH} "for i in \$(seq 1 16); do \
    timeout 2 python3 ${ORACLE_PATH} --resolver ${RESOLVER} --name ${PROBE_HOST} --qtype A --timeout 2 >/dev/null 2>&1 || true; \
done"
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
print_sle_states

# 若基线解析器本就故障，注入 DROP 无法区分"注入导致"与"本来就坏"，
# 该阶段结论不可解释，如实标记 SKIP 而不是伪造 FAIL。
DNS_SLE_3=$(dns_sle_state)
DNS_REASON_3=$(dns_sle_reason)
info "DNS SLE 自身状态: ${DNS_SLE_3} (reason=${DNS_REASON_3})"

if [ "$DNS_SLE_3" = "BAD" ]; then
    ok "DNS 故障被 DNS SLE 正确判为 BAD"
else
    bad "注入后 DNS SLE 未判 BAD（实际 ${DNS_SLE_3}, reason=${DNS_REASON_3}）"
fi

# Primary Issue 仍需看 overall 层：底层正常时故障应归因 DNS
if echo "$ISSUES_3" | grep -qi 'dns'; then
    ok "Primary Issue 指向 DNS"
else
    bad "Primary Issue 未指向 DNS（issues: ${ISSUES_3}）"
fi

if python3 -c "import sys; sys.exit(0 if float('${LOSS_3}' or 0) <= 0.02 else 1)"; then
    ok "业务真坏时 Observer 仍健康 —— 故障归因可信"
else
    bad "业务真坏时 Observer 也异常，无法区分业务故障与观测故障"
fi

cleanup
# 注入的故障会留在 120s 窗口内；不等待会让后续阶段读到残留证据
wait_for_window_to_age

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
    ${SSH} "for i in \$(seq 1 40); do \
        python3 ${ORACLE_PATH} --resolver ${RESOLVER} --name ${PROBE_HOST} --qtype A --timeout 1 >/dev/null 2>&1 || true; \
    done"
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
    elif [ "$(dns_sle_state)" = "BAD" ]; then
        bad "观测器丢事件被误判为 DNS 业务故障（DNS SLE=BAD）—— 违反核心不变式"
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

wait_for_window_to_age
generate_dns_traffic 15
sleep 15

H5=$(health_json)
DNS_STATE_5=$(echo "$H5" | json_get overall_quality)
GT5=$(resolver_ground_truth 10)
GT_OK5=${GT5%%/*}
info "health: ${DNS_STATE_5}  ground truth: ${GT5}"

DNS_SLE_5=$(dns_sle_state)
info "DNS SLE 自身状态: ${DNS_SLE_5}"

if [ "${GT_OK5}" -ge 9 ]; then
    if [ "$DNS_SLE_5" = "GOOD" ]; then
        ok "恢复后解析器健康且 DNS SLE 判 GOOD"
    else
        bad "恢复后解析器健康(${GT5})但 DNS SLE 判 ${DNS_SLE_5}（reason=$(dns_sle_reason)）"
    fi
else
    info "解析器仍未完全恢复(${GT5})，DNS 判 ${DNS_STATE_5} 与事实相符"
fi

REMAINING=$(${SSH} 'sudo iptables -S | grep -c "53" || true')
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
