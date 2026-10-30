#!/usr/bin/env bash
# ============================================================================
# OTA P0-C1 启动门禁 · 沙盒演练（不碰真机、不启 systemd）
#
# 单元测试（test_weaknet_ota_gate.py）已经覆盖 decide() 与接线，但**没有**
# 覆盖"门禁与真实升级流程是否兼容"。这里补的正是这一类端到端缺陷：
#
#   门禁曾经在 TRIAL / ROLLBACK_VERIFY 一律拒绝 → 升级脚本 do-switch 之后
#   的 restart 被挡 → 健康窗口必然超时 → 回滚重启又被挡 → 升级 100% 失败。
#   单元测试当时是"绿的"，因为它把期望写成了"拒绝"。这说明只测守卫本身
#   不够，必须把守卫接到**真实的阶段推进序列**上再跑一遍。
#
# 断言分三组：
#   A. 阶段矩阵：终态/无事务/活跃阶段放行；救援态/损坏/未知阶段拒绝
#   B. 升级全流程重放：从 bootstrap 到 COMMITTED，每个阶段门禁都必须放行
#   C. 救援链：进入救援后门禁拒绝，人工 recover --accept 后放行
#
# 用法：bash tools/ota_p0c1_drill.sh
# ============================================================================
set -uo pipefail

ROOT_REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# OTA_GATE_BIN 只用于验证"演练本身是否有效"：拿一个故意改坏的副本跑，
# 演练必须报红。正常使用不要设置它。
GATE="${OTA_GATE_BIN:-${ROOT_REPO}/tools/weaknet-ota-gate}"
CTL="${ROOT_REPO}/tools/weaknet-ota-ctl"
WORK="$(mktemp -d /tmp/wn-p0c1-XXXXXX)"
BROOT="${WORK}/root"
STATE="${WORK}/ota-state"
mkdir -p "${BROOT}/releases" "${STATE}"

PASS=0
FAIL=0
ok()   { PASS=$((PASS + 1)); printf '  \033[32mPASS\033[0m %s\n' "$1"; }
bad()  { FAIL=$((FAIL + 1)); printf '  \033[31mFAIL\033[0m %s\n' "$1"; }
sect() { printf '\n\033[36m== %s ==\033[0m\n' "$1"; }

cleanup() { rm -rf "${WORK}"; }
trap cleanup EXIT

ctl()  { python3 "${CTL}" --root "${BROOT}" --state-dir "${STATE}" "$@" 2>/dev/null; }
gate() { python3 "${GATE}" --root "${BROOT}" --state-dir "${STATE}" 2>/dev/null; }

# 造一个带 .committed 的版本目录（控制器只要求目录与标记存在）
mkver() {
    mkdir -p "${BROOT}/releases/$1"
    : > "${BROOT}/releases/$1/.committed"
}
set_current() {
    ln -sfn "releases/$1" "${BROOT}/current.tmp"
    mv -T "${BROOT}/current.tmp" "${BROOT}/current"
}

# 断言门禁判定，$1=描述 $2=期望 allow(true/false)
expect_allow() {
    local desc="$1" want="$2" out allow
    out="$(gate)"
    allow="$(printf '%s' "${out}" | grep -oE '"allow": (true|false)' | grep -oE 'true|false')"
    if [ "${allow}" = "${want}" ]; then
        ok "${desc}（allow=${allow}）"
    else
        bad "${desc}：期望 allow=${want}，实际 allow=${allow:-空} / ${out}"
    fi
}

# ---------------------------------------------------------------------------
sect "A. 阶段矩阵"
# ---------------------------------------------------------------------------
rm -f "${STATE}/transaction.json"
expect_allow "无状态文件（全新设备）放行" true

mkver 1.0.0
set_current 1.0.0
ctl bootstrap >/dev/null 2>&1
expect_allow "bootstrap 之后放行" true

# 直接写状态文件构造各阶段（只测门禁判定，不测状态机）
write_phase() {
    python3 - "$1" <<'PY'
import json, os, sys
state_dir = os.environ["STATE"]
phase = sys.argv[1]
state = {
    "schema_version": 1, "revision": 1,
    "stable_version": "1.0.0", "previous_stable_version": None,
    "transaction": {
        "transaction_id": "drill", "kind": "upgrade",
        "phase": phase if phase != "NO_TX" else None,
        "from_version": "1.0.0", "to_version": "1.1.0",
        "rollback_target": "1.0.0", "previous_stable_before": None,
        "boot_attempts": 0, "last_trial_boot_id": "", "policy": {},
        "started_at": "2026-01-01T00:00:00Z",
        "updated_at": "2026-01-01T00:00:00Z", "last_error": "",
    },
}
if phase == "NO_TX":
    state["transaction"] = None
with open(os.path.join(state_dir, "transaction.json"), "w", encoding="utf-8") as fh:
    json.dump(state, fh)
PY
}
export STATE

for p in COMMITTED FORCED_COMMITTED ABORTED ROLLED_BACK NO_TX; do
    write_phase "$p"
    expect_allow "终态 ${p} 放行" true
done

# 活跃阶段必须放行——这是本次演练存在的理由
for p in PREPARED SWITCH_PENDING TRIAL ROLLBACK_PENDING ROLLBACK_VERIFY; do
    write_phase "$p"
    expect_allow "活跃阶段 ${p} 放行（健康窗口依赖服务在线）" true
done

write_phase RECOVERY_REQUIRED
expect_allow "救援态 RECOVERY_REQUIRED 拒绝" false

write_phase SOME_FUTURE_PHASE
expect_allow "未登记阶段拒绝" false

printf '{ broken json' > "${STATE}/transaction.json"
expect_allow "状态文件损坏拒绝" false

printf '{"schema_version": 99}' > "${STATE}/transaction.json"
expect_allow "schema 不认识拒绝" false

# 拒绝时退出码必须非零（systemd 只看这个）
printf '{ broken json' > "${STATE}/transaction.json"
rc=0; gate >/dev/null 2>&1 || rc=$?
if [ "${rc}" -ne 0 ]; then ok "损坏状态退出码非零（rc=${rc}）"; else bad "损坏状态退出码为 0"; fi

# ---------------------------------------------------------------------------
sect "B. 升级全流程重放：每个阶段门禁都必须放行"
# ---------------------------------------------------------------------------
rm -f "${STATE}/transaction.json" "${STATE}/rescue.flag"
rm -rf "${BROOT}/releases"; mkdir -p "${BROOT}/releases"
mkver 1.0.0
set_current 1.0.0
ctl bootstrap >/dev/null 2>&1
expect_allow "B0 bootstrap 后放行" true

# 造 1.1.0 的 staging 目录（begin 要求目标版本尚未落位时用 --staged 指定）
mkdir -p "${WORK}/staging/server/bin"
echo "binary" > "${WORK}/staging/server/bin/weaknet-dbus-server"

if ctl begin --kind upgrade --from 1.0.0 --to 1.1.0 --rollback-target 1.0.0 \
        --staged "${WORK}/staging" >/dev/null 2>&1; then
    ok "begin → PREPARED"
else
    bad "begin 失败"
fi
expect_allow "B1 PREPARED 放行" true

ctl materialize >/dev/null 2>&1
expect_allow "B2 materialize 后（仍 PREPARED）放行" true

ctl arm-switch >/dev/null 2>&1
expect_allow "B3 SWITCH_PENDING 放行" true

ctl do-switch >/dev/null 2>&1
expect_allow "B4 TRIAL 放行（升级脚本在此之后 restart 服务）" true

# 这一步是原缺陷的复现点：TRIAL 里重启服务必须能起来
phase_now="$(python3 "${CTL}" --root "${BROOT}" --state-dir "${STATE}" status 2>/dev/null \
             | grep -oE '"phase": *"[A-Z_]+"' | grep -oE '[A-Z_]+"$' | tr -d '"')"
if [ "${phase_now}" = "TRIAL" ]; then ok "do-switch 后阶段为 TRIAL"; else bad "do-switch 后阶段=${phase_now:-空}"; fi

ctl commit >/dev/null 2>&1
expect_allow "B5 COMMITTED 放行" true

# 回滚链：从 1.1.0 手动回滚到 1.0.0，同样每个阶段都要能启动服务
if ctl begin --kind manual_rollback --from 1.1.0 --to 1.0.0 \
        --rollback-target 1.1.0 >/dev/null 2>&1; then
    ok "回滚链 begin（1.1.0 → 1.0.0）"
else
    bad "回滚链 begin 失败"
fi
ctl arm-switch >/dev/null 2>&1
ctl do-switch >/dev/null 2>&1
expect_allow "B6 回滚链 TRIAL 放行" true
ctl rollback-arm --reason drill >/dev/null 2>&1
expect_allow "B7 ROLLBACK_PENDING 放行（回滚翻链后要重启服务验证）" true
ctl rollback-do >/dev/null 2>&1
expect_allow "B8 ROLLBACK_VERIFY 放行" true
ctl rolled-back >/dev/null 2>&1
expect_allow "B9 ROLLED_BACK 放行" true

# ---------------------------------------------------------------------------
sect "C. 救援链：进入拒绝，人工接受后放行"
# ---------------------------------------------------------------------------
ctl require-recovery --reason "drill" >/dev/null 2>&1
expect_allow "C1 require-recovery 后拒绝" false

if [ -f "${STATE}/rescue.flag" ]; then ok "C2 权威状态变更同时落了 rescue.flag"; else bad "C2 缺 rescue.flag"; fi

# 门禁不看 flag：删掉标记仍必须拒绝
rm -f "${STATE}/rescue.flag"
expect_allow "C3 删掉 rescue.flag 后仍拒绝（权威状态说了算）" false

ctl recover --accept 1.0.0 >/dev/null 2>&1
expect_allow "C4 recover --accept 后放行" true
if [ -f "${STATE}/rescue.flag" ]; then bad "C5 退出救援后 rescue.flag 未清除"; else ok "C5 退出救援后 rescue.flag 已清除"; fi

# flag 陈旧（状态正常但标记残留）：门禁仍放行（它是第二层防线，不是权威）
run_ok=1; set_current 1.0.0 >/dev/null 2>&1 || run_ok=0
printf '{"reason":"stale"}' > "${STATE}/rescue.flag"
expect_allow "C6 陈旧 rescue.flag 不影响门禁判定（标记不是权威）" true
rm -f "${STATE}/rescue.flag"

# ---------------------------------------------------------------------------
sect "结果"
# ---------------------------------------------------------------------------
printf '  PASS=%d  FAIL=%d\n' "${PASS}" "${FAIL}"
printf '  演练目录：%s\n' "${WORK}"
if [ "${FAIL}" -ne 0 ]; then
    echo "  演练未通过：门禁与升级流程不兼容（最常见原因：把活跃阶段改回拒绝）" >&2
    exit 1
fi
exit 0
