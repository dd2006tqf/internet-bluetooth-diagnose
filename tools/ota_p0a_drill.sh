#!/usr/bin/env bash
# ============================================================================
# P0-A 沙盒演练：在隔离目录里跑完整升级/回滚/崩溃恢复流程
#
# 不碰真机、不启服务：WEAKNET_UPDATE_DRYRUN=1 让 systemctl/busctl 变成空操作，
# 但**事务与指针逻辑照常真实执行**——这正是要验证的部分。
#
# 覆盖：
#   1. bootstrap 接管现有布局
#   2. 正常升级 → 健康 → 转正（COMMITTED）
#   3. 健康失败 → 自动回滚（ROLLED_BACK），失败版本不进 .previous
#   4. 同版本幂等重装（RENAME_EXCHANGE 原子交换）
#   5. 每个崩溃点：构造持久事实 → reconcile 判定
#   6. GC 在事务未完成时整体跳过
# ============================================================================
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$(mktemp -d /tmp/wn-p0a-XXXXXX)"
ROOT="$WORK/weaknet"
STATE="$WORK/ota-state"
export WEAKNET_ROOT="$ROOT"
export WEAKNET_OTA_STATE_DIR="$STATE"
export WEAKNET_OTA_CTL="$REPO/tools/weaknet-ota-ctl"
export WEAKNET_UPDATE_DRYRUN=1

PASS=0; FAIL=0
ok()   { PASS=$((PASS+1)); printf '  \033[32mPASS\033[0m %s\n' "$1"; }
bad()  { FAIL=$((FAIL+1)); printf '  \033[31mFAIL\033[0m %s\n' "$1"; }
head_() { printf '\n\033[36m== %s ==\033[0m\n' "$1"; }

check() {  # $1=描述 $2=期望 $3=实际
    if [ "$2" = "$3" ]; then ok "$1"; else bad "$1（期望 $2，实际 $3）"; fi
}

ctl() { python3 "$WEAKNET_OTA_CTL" --root "$ROOT" --state-dir "$STATE" "$@"; }
phase() { ctl status 2>/dev/null | sed -n 's/.*"phase": *"\([^"]*\)".*/\1/p'; }
curver() { readlink "$ROOT/current" 2>/dev/null | sed 's|^releases/||'; }

# ---- 构造初始布局：1.0.0 已转正 ----
mk_release() {  # $1=version
    local v="$1" d="$ROOT/releases/$1"
    mkdir -p "$d/server/bin"
    echo "$v" > "$d/VERSION"
    echo "binary-$v" > "$d/server/bin/weaknet-dbus-server"
    ( cd "$d" && find . -type f ! -name manifest.sha256 ! -name .committed -print0 \
        | sed -z 's|^\./||' | sort -z | xargs -0 sha256sum > manifest.sha256 )
}

mkdir -p "$ROOT/releases" "$STATE"
mk_release 1.0.0
touch "$ROOT/releases/1.0.0/.committed"
ln -sfn releases/1.0.0 "$ROOT/current"
ln -sfn current/server "$ROOT/server"

head_ "1. bootstrap 接管现有布局"
ctl bootstrap >/dev/null 2>&1
check "bootstrap 后 stable_version=1.0.0" "1.0.0" \
      "$(ctl status | sed -n 's/.*"stable_version": *"\([^"]*\)".*/\1/p')"
check "bootstrap 后无进行中事务" "" "$(phase)"

head_ "2. 正常升级 1.0.0 → 1.1.0（走完整状态机）"
mk_release 1.1.0
# 模拟 apply：staging 已校验 → begin(staged) 必须指向尚未落位的目录
STAGE="$WORK/stage-1.1.0"; mkdir -p "$STAGE/server/bin"
echo "1.1.0" > "$STAGE/VERSION"; echo "binary-1.1.0" > "$STAGE/server/bin/weaknet-dbus-server"
( cd "$STAGE" && find . -type f ! -name manifest.sha256 ! -name .committed -print0 \
    | sed -z 's|^\./||' | sort -z | xargs -0 sha256sum > manifest.sha256 )
rm -rf "$ROOT/releases/1.1.0"          # 尚未落位
ctl begin --kind upgrade --from 1.0.0 --to 1.1.0 --rollback-target 1.0.0 \
    --staged "$STAGE" >/dev/null 2>&1
check "begin 后 phase=PREPARED" "PREPARED" "$(phase)"
ctl materialize >/dev/null 2>&1
check "materialize 后目标目录就位" "yes" "$([ -d "$ROOT/releases/1.1.0" ] && echo yes || echo no)"
check "materialize 不动指针" "1.0.0" "$(curver)"
ctl arm-switch >/dev/null 2>&1
check "arm-switch 后 phase=SWITCH_PENDING" "SWITCH_PENDING" "$(phase)"
check "arm-switch 不动指针" "1.0.0" "$(curver)"
ctl do-switch >/dev/null 2>&1
check "do-switch 后 phase=TRIAL" "TRIAL" "$(phase)"
check "do-switch 翻指针" "1.1.0" "$(curver)"
ctl commit >/dev/null 2>&1
check "commit 后 phase=COMMITTED" "COMMITTED" "$(phase)"
check "stable 更新为 1.1.0" "1.1.0" \
      "$(ctl status | sed -n 's/.*"stable_version": *"\([^"]*\)".*/\1/p')"
check "previous 更新为 1.0.0" "1.0.0" \
      "$(ctl status | sed -n 's/.*"previous_stable_version": *"\([^"]*\)".*/\1/p')"
check ".committed 投影已补写" "yes" \
      "$([ -f "$ROOT/releases/1.1.0/.committed" ] && echo yes || echo no)"
check ".previous 投影=1.0.0" "1.0.0" "$(cat "$ROOT/releases/.previous" 2>/dev/null | tr -d '\n')"
check "current 仍是相对路径" "releases/1.1.0" "$(readlink "$ROOT/current")"

head_ "3. 健康失败 → 自动回滚（候选 1.2.0 不得成为 previous）"
mk_release 1.2.0
ctl begin --kind upgrade --from 1.1.0 --to 1.2.0 --rollback-target 1.1.0 >/dev/null 2>&1
ctl arm-switch >/dev/null 2>&1
ctl do-switch >/dev/null 2>&1
check "试运行中 current=1.2.0" "1.2.0" "$(curver)"
ctl rollback-arm --reason "健康窗口未通过" >/dev/null 2>&1
check "rollback-arm 后不立即改指针" "1.2.0" "$(curver)"
ctl rollback-do >/dev/null 2>&1
check "rollback-do 翻回 1.1.0" "1.1.0" "$(curver)"
ctl rolled-back >/dev/null 2>&1
check "rolled-back 后 phase=ROLLED_BACK" "ROLLED_BACK" "$(phase)"
check "失败候选 1.2.0 不是 stable" "1.1.0" \
      "$(ctl status | sed -n 's/.*"stable_version": *"\([^"]*\)".*/\1/p')"
check "失败候选 1.2.0 不是 previous" "1.0.0" \
      "$(ctl status | sed -n 's/.*"previous_stable_version": *"\([^"]*\)".*/\1/p')"

head_ "4. 同版本幂等重装（RENAME_EXCHANGE 原子交换）"
echo "OLD-MARKER" > "$ROOT/releases/1.1.0/OLD_MARKER"
STAGE2="$WORK/stage-1.1.0b"; mkdir -p "$STAGE2/server/bin"
echo "1.1.0" > "$STAGE2/VERSION"
echo "NEW-MARKER" > "$STAGE2/NEW_MARKER"
ctl begin --kind upgrade --from 1.1.0 --to 1.1.0 --rollback-target 1.1.0 \
    --staged "$STAGE2" >/dev/null 2>&1
RC=$?
if [ "$RC" -eq 0 ]; then
    ctl materialize >/dev/null 2>&1
    check "新内容已就位" "yes" "$([ -f "$ROOT/releases/1.1.0/NEW_MARKER" ] && echo yes || echo no)"
    check "旧内容被换到 staging" "yes" "$([ -f "$STAGE2/OLD_MARKER" ] && echo yes || echo no)"
    check "交换后 current 仍有效" "1.1.0" "$(curver)"
    check "交换后 current 目录可访问" "yes" \
          "$([ -d "$ROOT/current/server" ] && echo yes || echo no)"
    ctl arm-switch >/dev/null 2>&1
    ctl do-switch >/dev/null 2>&1
    ctl commit >/dev/null 2>&1
    # 同版本重装后 previous 不得等于 current —— 否则无参回滚会因
    # 「目标与当前相同」失败，设备少了一条自动退路。
    check "同版本转正后 previous≠current" "not-equal" \
          "$([ "$(ctl status | sed -n 's/.*"previous_stable_version": *"\([^"]*\)".*/\1/p')" != "1.1.0" ] \
             && echo not-equal || echo equal)"
else
    bad "同版本 begin 失败（rc=$RC）"
fi

head_ "5. 崩溃点判定（构造持久事实 → reconcile --dry-run，不执行任何动作）"
check_crash() {  # $1=phase $2=current版本 $3=期望 action
    local f="$STATE/transaction.json"
    # 每个崩溃点都要用一套**固定**的 from/to/rollback 事实，否则会拿上一笔
    # 事务残留的版本号去对照，判定结果无意义（演练第一版就是这么误报的）。
    python3 - "$f" "$1" <<'PY'
import json, sys
path, phase = sys.argv[1], sys.argv[2]
d = json.load(open(path))
d["transaction"] = {
    "transaction_id": "crash-test", "kind": "upgrade", "phase": phase,
    "from_version": "1.0.0", "to_version": "1.1.0",
    "rollback_target": "1.0.0", "previous_stable_before": None,
    "boot_attempts": 0, "last_trial_boot_id": "", "policy": {},
    "started_at": "", "updated_at": "", "last_error": "",
}
json.dump(d, open(path, "w"))
PY
    ln -sfn "releases/$2" "$ROOT/current"
    local act
    act="$(ctl reconcile --dry-run 2>/dev/null | sed -n 's/.*"action": *"\([^"]*\)".*/\1/p')"
    check "  ${1} + current=${2} → ${3}" "$3" "$act"
}

check_crash PREPARED         1.0.0 abort
check_crash SWITCH_PENDING   1.0.0 abort
check_crash SWITCH_PENDING   1.1.0 mark_trial
check_crash TRIAL            1.1.0 continue_trial
check_crash TRIAL            1.0.0 recovery_required
check_crash ROLLBACK_PENDING 1.1.0 continue_rollback
check_crash ROLLBACK_PENDING 1.0.0 continue_rollback
check_crash ROLLBACK_VERIFY  1.0.0 continue_trial
check_crash RECOVERY_REQUIRED 1.1.0 recovery_required

# 首次安装例外：设备还没有 current 软链（cur is None），from == to。
# 判定不得因此判成 recovery_required（那会让新设备永远装不上第一个版本）。
python3 - "$STATE/transaction.json" <<'PY'
import json, sys
p = sys.argv[1]; d = json.load(open(p))
d["transaction"] = {
    "transaction_id": "crash-fresh", "kind": "upgrade", "phase": "PREPARED",
    "from_version": "2.0.0", "to_version": "2.0.0",
    "rollback_target": "2.0.0", "previous_stable_before": None,
    "boot_attempts": 0, "last_trial_boot_id": "", "policy": {},
    "started_at": "", "updated_at": "", "last_error": "",
}
json.dump(d, open(p, "w"))
PY
rm -f "$ROOT/current"
FRESH_ACT="$(ctl reconcile --dry-run 2>/dev/null | sed -n 's/.*"action": *"\([^"]*\)".*/\1/p')"
check "  首次安装 PREPARED + 无 current → abort" "abort" "$FRESH_ACT"

head_ "6. GC 在事务未完成时整体跳过"
ln -sfn releases/1.1.0 "$ROOT/current"
for v in 1.3.0 1.4.0 1.5.0; do mk_release "$v"; done
cp "$STATE/transaction.json" "$WORK/tx-backup.json"
python3 - "$STATE/transaction.json" <<'PY'
import json, sys
p = sys.argv[1]; d = json.load(open(p))
d["transaction"]["phase"] = "TRIAL"
json.dump(d, open(p, "w"))
PY
BEFORE=$(ls -d "$ROOT"/releases/[0-9]*/ 2>/dev/null | wc -l)
ctl gc --max-keep 1 >/dev/null 2>&1
AFTER=$(ls -d "$ROOT"/releases/[0-9]*/ 2>/dev/null | wc -l)
check "非终态 GC 不删任何版本（$BEFORE → $AFTER）" "$BEFORE" "$AFTER"

head_ "7. schema 损坏 → fail-closed"
echo '{ broken json' > "$STATE/transaction.json"
OUT=$(ctl status 2>&1)
check "损坏 JSON 被拒绝（exit 5）" "5" "$?"
echo "$OUT" | grep -q "损坏" && ok "报错信息指明数据损坏" || bad "报错信息不明确：$OUT"

head_ "8. 首次安装（无 current）：from == to == 目标版本"
# 全新设备：还没有 current，也就没有任何可回滚版本。升级脚本会把
# from 与 rollback-target 都取成目标版本（控制器的同版本例外）。
FROOT="$WORK/fresh"
FSTATE="$WORK/fresh-state"
mkdir -p "$FROOT/releases" "$FSTATE"
STAGE3="$WORK/stage-fresh"; mkdir -p "$STAGE3/server/bin"
echo "2.0.0" > "$STAGE3/VERSION"; echo "binary-2.0.0" > "$STAGE3/server/bin/weaknet-dbus-server"
fctl() { python3 "$WEAKNET_OTA_CTL" --root "$FROOT" --state-dir "$FSTATE" "$@"; }
fctl begin --kind upgrade --from 2.0.0 --to 2.0.0 --rollback-target 2.0.0 \
    --staged "$STAGE3" >/dev/null 2>&1
check "首次安装 begin 被接受" "0" "$?"
fctl materialize >/dev/null 2>&1
check "首次安装 materialize 就位" "yes" \
      "$([ -d "$FROOT/releases/2.0.0" ] && echo yes || echo no)"
fctl arm-switch >/dev/null 2>&1
fctl do-switch >/dev/null 2>&1
check "首次安装指针指向新版本" "2.0.0" "$(readlink "$FROOT/current" | sed 's|^releases/||')"
fctl commit >/dev/null 2>&1
check "首次安装转正" "2.0.0" \
      "$(fctl status | sed -n 's/.*"stable_version": *"\([^"]*\)".*/\1/p')"
check "首次安装无上一稳定版本" "" \
      "$(fctl status | sed -n 's/.*"previous_stable_version": *"\([^"]*\)".*/\1/p')"

printf '\n\033[36m== 结果 ==\033[0m\n'
printf '  PASS=%d  FAIL=%d\n' "$PASS" "$FAIL"
printf '  演练目录：%s\n' "$WORK"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
