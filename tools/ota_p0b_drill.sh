#!/usr/bin/env bash
# ============================================================================
# P0-B 沙盒演练：开机恢复服务的完整行为链
#
# 不碰真机、不启服务：用假 systemctl/busctl 替身（P0-B_DRILL_BIN 目录）
# 让恢复器面对各种持久事实，验证它给出的动作与实际后果。
#
# 覆盖：
#   1. 无事务 → 什么都不做
#   2. TRIAL + 健康 → 经控制器转正
#   3. TRIAL + 不健康 + 尝试次数耗尽 → 记录回滚意图 → 翻回旧版
#   4. TRIAL + 崩溃循环 → 立即回滚（不等窗口）
#   5. 控制器缺失 → 退回直接原子翻链（仍能救回来）
#   6. 状态损坏 → 救援模式（停服+mask，保留 SSH 与事务证据）
#   7. 救援模式报告内容完整
#   8. 恢复器源码不引用升级脚本（升级脚本消失也能恢复）
# ============================================================================
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$(mktemp -d /tmp/wn-p0b-XXXXXX)"
ROOT="$WORK/weaknet"
STATE="$WORK/ota-state"
FAKEBIN="$WORK/bin"

export WEAKNET_ROOT="$ROOT"
export WEAKNET_OTA_STATE_DIR="$STATE"
export WEAKNET_OTA_CTL="$FAKEBIN/weaknet-ota-ctl"
# 假 systemctl/busctl 必须在 PATH 最前面——恢复器是用绝对命令名调的
# （systemctl / busctl），不靠 PATH 就会打到真的系统上去。
export PATH="$FAKEBIN:$PATH"

PASS=0; FAIL=0
ok()   { PASS=$((PASS+1)); printf '  \033[32mPASS\033[0m %s\n' "$1"; }
bad()  { FAIL=$((FAIL+1)); printf '  \033[31mFAIL\033[0m %s\n' "$1"; }
head_() { printf '\n\033[36m== %s ==\033[0m\n' "$1"; }
check() { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1（期望 $2，实际 $3）"; fi; }

# ---- 假 systemctl / busctl ----
# 用环境变量控制假行为：
#   FAKE_SERVICE_STATE  active|inactive|failed
#   FAKE_BUS_STATE       up|down
#   FAKE_RESTARTS        数字
#   FAKE_INSTALLED       GetUpdateStatus 报告的版本
mk_fakebin() {
    mkdir -p "$FAKEBIN"
    cat > "$FAKEBIN/systemctl" <<'EOF'
#!/usr/bin/env bash
cmd="$1"; shift
case "$cmd" in
    is-active)
        unit="$1"
        echo "$cmd $*" >> "$FAKE_LOG"
        if [ "$unit" = "weaknet-server" ]; then
            echo "${FAKE_SERVICE_STATE:-active}"
        else
            echo "${FAKE_UNIT_STATE:-active}"
        fi
        exit 0 ;;
    show)
        # systemctl show <unit> -p NRestarts --value
        echo "$cmd $*" >> "$FAKE_LOG"
        echo "${FAKE_RESTARTS:-0}"
        exit 0 ;;
    stop|mask|disable|enable|start|restart|daemon-reload)
        echo "$cmd $*" >> "$FAKE_LOG"
        exit 0 ;;
    *) exit 0 ;;
esac
EOF
    cat > "$FAKEBIN/busctl" <<'EOF'
#!/usr/bin/env bash
if [ "${FAKE_BUS_STATE:-up}" != "up" ]; then
    exit 1
fi
case "$*" in
    *GetUpdateStatus*) echo 's "{\"installed_version\":\"'"${FAKE_INSTALLED:-1.1.0}"'\"}"' ;;
    *) echo 's "ok"' ;;
esac
exit 0
EOF
    chmod +x "$FAKEBIN/systemctl" "$FAKEBIN/busctl"
}

# ---- 假控制器：把子命令记进 ctl.log，返回 0 ----
mk_fake_ctl() {
    # 假控制器必须是 **Python** 脚本：真实控制器就是这样被调用的
    # （恢复器用 sys.executable 启动它，因为文件没有 .py 后缀）。
    # 写成 bash 会被 python3 当成 Python 源码解析而失败。
    cat > "$FAKEBIN/weaknet-ota-ctl" <<EOF
#!/usr/bin/env python3
import json, os, sys

STATE_DIR = "$STATE"
ROOT_DIR = "$ROOT"
LOG = "$WORK/ctl.log"

argv = sys.argv[1:]
sub = None
i = 0
while i < len(argv):
    a = argv[i]
    if a in ("--root", "--state-dir"):
        i += 2
        continue
    if a.startswith("--"):
        i += 1
        continue
    sub = a
    break

with open(LOG, "a") as fh:
    fh.write((sub or "") + "\n")

state_path = os.path.join(STATE_DIR, "transaction.json")


def load():
    with open(state_path) as fh:
        return json.load(fh)


def save(d):
    tmp = state_path + ".tmp"
    with open(tmp, "w") as fh:
        json.dump(d, fh)
    os.replace(tmp, state_path)


def set_current(ver):
    link = os.path.join(ROOT_DIR, "current")
    if os.path.lexists(link):
        os.unlink(link)
    os.symlink("releases/" + ver, link)


if sub == "bump-boot-attempt":
    d = load()
    d["transaction"]["boot_attempts"] = int(d["transaction"].get("boot_attempts", 0)) + 1
    save(d)
    print(json.dumps({"ok": True, "boot_attempts": d["transaction"]["boot_attempts"]}))
elif sub == "bump-trial-round":
    # 与真控制器一致：**不按 boot_id 幂等**，并刷新窗口起点。
    import time as _t
    d = load()
    d["transaction"]["boot_attempts"] = int(d["transaction"].get("boot_attempts", 0)) + 1
    d["transaction"]["updated_at"] = _t.strftime("%Y-%m-%dT%H:%M:%SZ", _t.gmtime())
    save(d)
    print(json.dumps({"ok": True, "boot_attempts": d["transaction"]["boot_attempts"]}))
elif sub == "do-switch":
    d = load()
    set_current(d["transaction"]["to_version"])
    d["transaction"]["phase"] = "TRIAL"
    save(d)
    print(json.dumps({"ok": True, "phase": "TRIAL"}))
elif sub == "commit":
    d = load()
    d["transaction"]["phase"] = "COMMITTED"
    d["stable_version"] = d["transaction"]["to_version"]
    save(d)
    print(json.dumps({"ok": True, "phase": "COMMITTED"}))
elif sub == "abort":
    d = load()
    d["transaction"]["phase"] = "ABORTED"
    save(d)
    print(json.dumps({"ok": True, "phase": "ABORTED"}))
elif sub == "rollback-arm":
    d = load()
    d["transaction"]["phase"] = "ROLLBACK_PENDING"
    save(d)
    print(json.dumps({"ok": True, "phase": "ROLLBACK_PENDING"}))
elif sub == "rollback-do":
    d = load()
    set_current(d["transaction"]["rollback_target"])
    d["transaction"]["phase"] = "ROLLBACK_VERIFY"
    save(d)
    print(json.dumps({"ok": True, "phase": "ROLLBACK_VERIFY"}))
elif sub == "rolled-back":
    d = load()
    d["transaction"]["phase"] = "ROLLED_BACK"
    d["stable_version"] = d["transaction"]["rollback_target"]
    save(d)
    print(json.dumps({"ok": True, "phase": "ROLLED_BACK"}))
else:
    print(json.dumps({"ok": True}))
EOF
    chmod +x "$FAKEBIN/weaknet-ota-ctl"
}

# ---- 布局与状态构造 ----
mk_release() {
    local v="$1" d="$ROOT/releases/$1"
    mkdir -p "$d/server/bin"
    echo "$v" > "$d/VERSION"
    echo "binary-$v" > "$d/server/bin/weaknet-dbus-server"
}
curver() { readlink "$ROOT/current" 2>/dev/null | sed 's|^releases/||'; }
phase() { python3 -c "
import json,sys
d=json.load(open('$STATE/transaction.json'))
print(d['transaction']['phase'] if d.get('transaction') else '')
"; }
attempts() { python3 -c "
import json
d=json.load(open('$STATE/transaction.json'))
print(d['transaction'].get('boot_attempts',0) if d.get('transaction') else 0)
"; }
write_state() {  # $1=phase $2=to $3=rollback $4=attempts
    python3 - "$STATE/transaction.json" "$1" "$2" "$3" "$4" <<'PY'
import json, sys
p, phase, to, rb, att = sys.argv[1:6]
d = {
  "schema_version": 1, "revision": 1,
  "stable_version": "1.0.0", "previous_stable_version": None,
  "transaction": {
    "transaction_id": "drill-tx", "kind": "upgrade", "phase": phase,
    "from_version": "1.0.0", "to_version": to, "rollback_target": rb,
    "previous_stable_before": None, "boot_attempts": int(att),
    "last_trial_boot_id": "", "policy": {"health_timeout": 120,
    "health_ok_needed": 3, "health_poll_s": 5},
    "started_at": "2026-01-01T00:00:00Z", "updated_at": "2026-01-01T00:00:00Z",
    "last_error": "",
  },
}
json.dump(d, open(p, "w"))
PY
}

recover() { python3 "$REPO/tools/weaknet-ota-recover" --root "$ROOT" \
    --state-dir "$STATE" --ctl "$WEAKNET_OTA_CTL" --once 2>&1; }

mkdir -p "$ROOT/releases" "$STATE"
mk_release 1.0.0
mk_release 1.1.0
mk_release 1.2.0
ln -sfn releases/1.0.0 "$ROOT/current"
mk_fakebin
mk_fake_ctl
export FAKE_LOG="$WORK/systemctl.log"
: > "$FAKE_LOG"

head_ "1. 无事务 → 什么都不做"
rm -f "$STATE/transaction.json"
OUT="$(recover)"
# 只读动作（is-active / show）不算副作用；有副作用的只有 stop/mask/
# start/restart/daemon-reload
check "无事务时不产生任何 systemctl 副作用动作" "0" \
      "$(grep -cE '^(stop|mask|disable|enable|start|restart|daemon-reload) ' "$FAKE_LOG" || true)"
echo "$OUT" | grep -q "没有进行中的事务" && ok "日志说明无事务" || bad "日志不对：$OUT"

head_ "2. TRIAL + 健康 → 经控制器转正"
write_state TRIAL 1.1.0 1.0.0 0
ln -sfn releases/1.1.0 "$ROOT/current"
export FAKE_SERVICE_STATE=active FAKE_BUS_STATE=up FAKE_INSTALLED=1.1.0
export FAKE_RESTARTS=0
OUT="$(recover)"
check "转正后 phase=COMMITTED" "COMMITTED" "$(phase)"
check "转正走的是控制器 commit" "yes" \
      "$(grep -qx commit "$WORK/ctl.log" && echo yes || echo no)"
check "试运行前已计数" "1" "$(attempts)"

head_ "3. TRIAL + 不健康 + 尝试次数耗尽 → 回滚"
write_state TRIAL 1.2.0 1.0.0 3
ln -sfn releases/1.2.0 "$ROOT/current"
: > "$WORK/ctl.log"
export FAKE_SERVICE_STATE=active FAKE_BUS_STATE=down FAKE_INSTALLED=1.2.0
export FAKE_RESTARTS=0
OUT="$(recover)"
check "记录了回滚意图" "yes" \
      "$(grep -qx rollback-arm "$WORK/ctl.log" && echo yes || echo no)"

head_ "4. TRIAL + 崩溃循环 → 立即回滚（不等窗口）"
write_state TRIAL 1.2.0 1.0.0 0
ln -sfn releases/1.2.0 "$ROOT/current"
: > "$WORK/ctl.log"
export FAKE_SERVICE_STATE=active FAKE_BUS_STATE=down FAKE_INSTALLED=1.2.0
export FAKE_RESTARTS=5
OUT="$(recover)"
check "崩溃循环触发回滚意图" "yes" \
      "$(grep -qx rollback-arm "$WORK/ctl.log" && echo yes || echo no)"
echo "$OUT" | grep -q "崩溃循环" && ok "日志指明崩溃循环" || bad "日志未指明：$OUT"

head_ "5. 控制器缺失 → 退回直接原子翻链"
mv "$FAKEBIN/weaknet-ota-ctl" "$WORK/ctl.bak"
write_state ROLLBACK_PENDING 1.2.0 1.0.0 1
ln -sfn releases/1.2.0 "$ROOT/current"
OUT="$(recover)"
check "控制器缺失时仍翻回旧版" "1.0.0" "$(curver)"
echo "$OUT" | grep -q "直接原子翻链" && ok "显式告警走了兜底路径" || bad "未告警：$OUT"
mv "$WORK/ctl.bak" "$FAKEBIN/weaknet-ota-ctl"

head_ "6. 状态损坏 → 救援模式"
echo '{ broken json' > "$STATE/transaction.json"
: > "$FAKE_LOG"
OUT="$(recover)"
check "救援模式停服" "yes" \
      "$(grep -q 'stop weaknet-server' "$FAKE_LOG" && echo yes || echo no)"
check "救援模式阻止服务被拉起（mask 或 disable）" "yes" \
      "$(grep -qE '(mask|disable) weaknet-server' "$FAKE_LOG" && echo yes || echo no)"
check "救援报告已生成" "yes" \
      "$([ -f "$STATE/RESCUE.md" ] && echo yes || echo no)"
check "救援保住了 ssh" "yes" \
      "$(grep -qE '(start|is-active) ssh' "$FAKE_LOG" && echo yes || echo no)"
check "救援保住了反向隧道" "yes" \
      "$(grep -q 'weaknet-tunnel' "$FAKE_LOG" && echo yes || echo no)"

head_ "7. 救援报告内容完整"
if [ -f "$STATE/RESCUE.md" ]; then
    grep -q "不做版本切换" "$STATE/RESCUE.md" && ok "报告声明不做版本切换" || bad "报告缺声明"
    grep -q "不做无限重启" "$STATE/RESCUE.md" && ok "报告声明不做无限重启" || bad "报告缺声明"
    grep -q "人工恢复步骤" "$STATE/RESCUE.md" && ok "报告含人工恢复步骤" || bad "报告缺步骤"
    grep -q "RESCUE" "$STATE/RESCUE.md" && ok "报告非空" || bad "报告为空"
else
    bad "没有救援报告"
fi

head_ "8. 恢复器不引用升级脚本（升级脚本消失也能恢复）"
# 只看代码（排除模块 docstring 与纯注释行）
python3 - "$REPO/tools/weaknet-ota-recover" <<'PYEOF'
import ast, sys
path = sys.argv[1]
src = open(path, encoding="utf-8").read()
tree = ast.parse(src)
skip = set()
for node in ast.walk(tree):
    if isinstance(node, (ast.Module, ast.FunctionDef, ast.ClassDef)):
        body = getattr(node, "body", [])
        if body and isinstance(body[0], ast.Expr) \
                and isinstance(body[0].value, ast.Constant) \
                and isinstance(body[0].value.value, str):
            for ln in range(body[0].value.lineno, body[0].value.end_lineno + 1):
                skip.add(ln)
bad = []
for i, line in enumerate(src.splitlines(), start=1):
    if i in skip:
        continue
    if "weaknet-local-update.sh" in line:
        bad.append((i, line))
if bad:
    for i, line in bad:
        sys.stderr.write("L%d: %s\n" % (i, line))
    sys.exit(1)
sys.exit(0)
PYEOF
if [ $? -eq 0 ]; then
    ok "恢复器代码不引用升级脚本"
else
    bad "恢复器代码里出现了升级脚本引用"
fi
rm -f "$ROOT/weaknet-local-update.sh"
write_state TRIAL 1.1.0 1.0.0 0
ln -sfn releases/1.1.0 "$ROOT/current"
export FAKE_SERVICE_STATE=active FAKE_BUS_STATE=up FAKE_INSTALLED=1.1.0
export FAKE_RESTARTS=0
OUT="$(recover)"
check "没有升级脚本也能完成转正" "COMMITTED" "$(phase)"

head_ "9. 窗口耗尽 → 持久计数 +1 → 下一次判定真的变化（缺陷 C1）"
# 这一节直接驱动 loop()（`--once` 不经过 loop 的耗尽分支），验证四件事：
#   1. 耗尽时返回 75，且**退出前**已经调过 bump-trial-round；
#   2. 同一次开机（boot_id 不变）里再跑一轮，计数仍然 +1（不幂等）；
#   3. 计数推到上限后不再请求重启，而是走回滚——不会无界重启；
#   4. 控制器不可用时 fail-closed 进救援，绝不以 75 退出。
expire_window() {
    python3 - "$STATE/transaction.json" <<'PYEOF'
import json, sys
p = sys.argv[1]
d = json.load(open(p))
d["transaction"]["updated_at"] = "2020-01-01T00:00:00Z"
json.dump(d, open(p, "w"))
PYEOF
}

# 小驱动器：跑一轮 loop()，第一次休眠就抛哨兵收尾，避免真睡。
run_loop_once() {  # $1 = ctl 路径
    python3 - "$REPO/tools/weaknet-ota-recover" "$ROOT" "$STATE" "$1" <<'PYEOF'
import importlib.machinery, importlib.util, os, sys
path, root, state, ctl = sys.argv[1:5]
loader = importlib.machinery.SourceFileLoader("wn_rec_drill", path)
spec = importlib.util.spec_from_loader("wn_rec_drill", loader)
rec = importlib.util.module_from_spec(spec)
loader.exec_module(rec)


class _Slept(Exception):
    """哨兵：一旦进入休眠就抛出，把 loop() 从常驻循环里截断。"""


def _sleeper(_seconds):
    # 必须用**普通函数**抛哨兵，不能用生成器表达式。
    #
    # 旧写法 `(_ for _ in ()).throw(StopIteration())` 依赖一个版本相关的
    # 行为：Python 3.9 会把从生成器里逸出的 StopIteration 按 PEP 479
    # 转换成 RuntimeError（"generator raised StopIteration"），于是本函数
    # 上方的 `except StopIteration` 抓不到，rc 变成空串、断言失败；
    # Python 3.12 在本例这种同步 `throw()` 调用下不转换，于是"本机全绿"。
    # 结论：这不是产品缺陷，是演练脚手架的错误假设，且只在板端 3.9 暴露。
    # 改用自定义哨兵 + 普通函数后，两个版本语义完全一致。
    raise _Slept()


r = rec.Recoverer(root, state, ctl, sleeper=_sleeper)
w = rec.Watcher(r, once=False)
w.read_boot_id = lambda: "drill-boot-fixed"
_override = os.environ.get("WEAKNET_DRILL_MAX_ROUNDS")
if _override:
    rec.MAX_TRIAL_ROUNDS = int(_override)
try:
    rc = w.loop()
except _Slept:
    rc = "slept"
print("RC=%s" % (rc,))
PYEOF
}

write_state TRIAL 1.1.0 1.0.0 0
expire_window
: > "$WORK/ctl.log"
export FAKE_SERVICE_STATE=active FAKE_BUS_STATE=down FAKE_INSTALLED=1.1.0
export FAKE_RESTARTS=0

OUT1="$(run_loop_once "$WEAKNET_OTA_CTL")"
RC1="$(printf '%s' "$OUT1" | sed -n 's/^RC=//p' | tail -1)"
check "窗口耗尽且还有次数 → 退出码 75" "75" "$RC1"
check "退出前已调用 bump-trial-round" "yes" \
      "$(grep -qx bump-trial-round "$WORK/ctl.log" && echo yes || echo no)"
# 首轮：run_once 的 bump_boot_attempt(+1) 之后 loop 再 bump_trial_round(+1)
# 首轮：run_once 的 bump_boot_attempt(+1) 之后 loop 再 bump_trial_round(+1) = 2。
# 第二轮同 boot_id：bump_boot_attempt 幂等（+0），窗口耗尽计数 +1 = 3，
# 而 3 已达 MAX_TRIAL_ROUNDS —— 说明"再跑一轮"本身就该转回滚。
check "首轮后计数为 2（开机计数 + 窗口耗尽计数）" "2" "$(attempts)"

# 第二轮同 boot_id —— 这里只断言"计数继续增长"（窗口计数不幂等），
# 不断言退出码：此时计数已达上限，恢复器正确地转向回滚而不是重启，
# 那正是"有限次收敛"的预期终点（见下一段）。
OUT2="$(run_loop_once "$WEAKNET_OTA_CTL")"
RT2="$(printf '%s' "$OUT2" | sed -n 's/^RC=//p' | tail -1)"
check "同 boot_id 第二轮推进到上限后不再重启" "slept" "$RT2"
check "同 boot_id 第二轮计数继续增长（窗口计数不幂等）" "3" "$(attempts)"

write_state TRIAL 1.1.0 1.0.0 3
expire_window
: > "$WORK/ctl.log"
OUT3="$(run_loop_once "$WEAKNET_OTA_CTL")"
RC3="$(printf '%s' "$OUT3" | sed -n 's/^RC=//p' | tail -1)"
check "次数耗尽后不再请求重启（不返回 75）" "slept" "$RC3"
check "次数耗尽后走回滚意图" "yes" \
      "$(grep -qx rollback-arm "$WORK/ctl.log" && echo yes || echo no)"

write_state TRIAL 1.1.0 1.0.0 0
expire_window
rm -f "$STATE/RESCUE.md" "$STATE/rescue.flag"
OUT4="$(run_loop_once "$WORK/no-such-ctl")"
RC4="$(printf '%s' "$OUT4" | sed -n 's/^RC=//p' | tail -1)"
check "控制器不可用时不得以 75 退出（会变成无界重启）" "0" "$RC4"
check "控制器不可用时留下救援现场" "yes" \
      "$([ -f "$STATE/RESCUE.md" ] && echo yes || echo no)"


printf '\n\033[36m== 结果 ==\033[0m\n'
printf '  PASS=%d  FAIL=%d\n' "$PASS" "$FAIL"
printf '  演练目录：%s\n' "$WORK"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
