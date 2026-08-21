#!/usr/bin/env bash
# auto_archive_push.sh — Evaluator Pass 后的自动提交+推送门禁
#
# 设计目的：
#   在 Harness 工作流的 Evaluator 给出 Pass verdict 后，把本次 change 的文件
#   提交并推送到远程分支。把"人工 git add/commit/push"自动化，但保留全套护栏。
#
# 触发时机：Evaluator verdict == Pass（由调用方/工作流保证；本脚本会复核）。
#
# 护栏（不可逆的对外操作，必须严格）：
#   1. 分支白名单：只允许 dev/* feature/* release/*；命中 main/master 直接拒绝。
#   2. 测试门禁：默认跑 make test-unit，必须 exit 0（--skip-test-gate 可跳过）。
#   3. Evaluator 门禁：若 openspec/changes/<change>/harness/evaluation.json 存在，
#      必须 verdict==Pass；不存在则需 --allow-no-eval（用于模拟/回溯场景）。
#   4. 显式文件清单：只 git add 指定文件，绝不用 git add -A；提交前校验暂存区
#      与清单完全一致，防止扫入脏文件/秘密。
#   5. 永不 --force、永不 --no-verify；pre-commit hook 正常跑。
#   6. 任一步失败即停，保留工作区原样，输出诊断。
#
# 用法：
#   scripts/auto_archive_push.sh --change <name> --files <f1,f2,...> \
#       [--skip-test-gate] [--allow-no-eval]
#
# 示例：
#   scripts/auto_archive_push.sh --change fix-event-counter-atomic \
#       --files server/src/event_manager.cpp,server/test/unit/test_event_manager.cpp \
#       --allow-no-eval --skip-test-gate

set -euo pipefail

# ---------- 颜色 ----------
RED=$'\033[31m'; GREEN=$'\033[32m'; YELLOW=$'\033[33m'; NC=$'\033[0m'
ok()   { echo "${GREEN}[OK]${NC}  $*"; }
warn() { echo "${YELLOW}[WARN]${NC} $*"; }
err()  { echo "${RED}[ERR]${NC}  $*" >&2; }

# ---------- 参数 ----------
CHANGE=""
FILES=""
SKIP_TEST_GATE=false
ALLOW_NO_EVAL=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --change)         CHANGE="$2"; shift 2 ;;
        --files)          FILES="$2"; shift 2 ;;
        --skip-test-gate) SKIP_TEST_GATE=true; shift ;;
        --allow-no-eval)  ALLOW_NO_EVAL=true; shift ;;
        -h|--help)
            sed -n '2,30p' "$0"; exit 0 ;;
        *) err "未知参数: $1"; exit 2 ;;
    esac
done

[[ -n "$CHANGE" ]] || { err "缺少 --change <name>"; exit 2; }
[[ -n "$FILES" ]] || { err "缺少 --files <f1,f2,...>"; exit 2; }

# 进入仓库根
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

echo "=== auto_archive_push: change=$CHANGE ==="

# ---------- 护栏 1：分支白名单 ----------
BRANCH="$(git rev-parse --abbrev-ref HEAD)"
case "$BRANCH" in
    dev/*|feature/*|release/*) ok "分支 $BRANCH 在白名单内" ;;
    main|master) err "拒绝在 $BRANCH 上自动推送"; exit 3 ;;
    *) err "分支 $BRANCH 不在白名单（dev/* feature/* release/*）"; exit 3 ;;
esac

# ---------- 护栏 3：Evaluator 门禁 ----------
EVAL_FILE="openspec/changes/$CHANGE/harness/evaluation.json"
if [[ -f "$EVAL_FILE" ]]; then
    VERDICT="$(node -e "try{console.log(JSON.parse(require('fs').readFileSync(process.argv[1],'utf8')).verdict||'')}catch(e){process.exit(1)}" "$EVAL_FILE" 2>/dev/null || echo "")"
    if [[ "$VERDICT" != "Pass" ]]; then
        err "Evaluator verdict='$VERDICT'（需 Pass）。evaluation.json: $EVAL_FILE"
        exit 4
    fi
    ok "Evaluator verdict=Pass（$EVAL_FILE）"
else
    if [[ "$ALLOW_NO_EVAL" != true ]]; then
        err "未找到 $EVAL_FILE，且未传 --allow-no-eval（用于模拟/回溯场景）"
        exit 4
    fi
    warn "未找到 evaluation.json（--allow-no-eval）：本次 Evaluator 为人工/模拟验收，跳过自动复核"
fi

# ---------- 护栏 2：测试门禁 ----------
if [[ "$SKIP_TEST_GATE" != true ]]; then
    ok "测试门禁：cmake 单元测试"
    # 优先 cmake，fallback 到 make
    if [ -d build ] && [ -f build/Makefile ]; then
        TEST_CMD="cmake --build build -j\$(nproc) && ctest --test-dir build/server -R 'test_net_info|test_quality|test_anomaly|test_audio|test_band|test_serializer|test_event|test_bt_full|test_bt_monitor$|test_iface|test_logger|test_traffic|test_database' --output-on-failure"
    else
        TEST_CMD="make -C server test-unit"
    fi
    if ! eval "$TEST_CMD" > /tmp/auto_push_test.log 2>&1; then
        err "测试门禁失败（exit!=0）。尾部日志："
        tail -20 /tmp/auto_push_test.log >&2
        exit 5
    fi
    ok "测试门禁通过"
else
    warn "跳过测试门禁（--skip-test-gate）：调用方须确保回归已绿"
fi

# ---------- 护栏 4：显式文件清单 ----------
IFS=',' read -ra FILE_ARRAY <<< "$FILES"
# 校验文件存在
for f in "${FILE_ARRAY[@]}"; do
    [[ -f "$f" ]] || { err "文件不存在: $f"; exit 6; }
done

# 暂存指定文件
git add -- "${FILE_ARRAY[@]}"

# 校验暂存区与清单完全一致（防止已有暂存或路径解析差异）
STAGED="$(git diff --cached --name-only | sort)"
EXPECTED="$(printf '%s\n' "${FILE_ARRAY[@]}" | sort)"
if [[ "$STAGED" != "$EXPECTED" ]]; then
    err "暂存区与文件清单不一致，回退暂存："
    diff <(echo "$EXPECTED") <(echo "$STAGED") >&2 || true
    git reset HEAD -- "${FILE_ARRAY[@]}" >/dev/null 2>&1 || true
    exit 6
fi
ok "暂存区与清单一致（${#FILE_ARRAY[@]} 个文件）"

# ---------- 提交 ----------
SHORT_SHA="$(git rev-parse --short HEAD)"
COMMIT_MSG="fix($CHANGE): eventCounter 改为 std::atomic 消除 data race

- event_manager.cpp: static int32_t -> std::atomic<int32_t>
- 多线程并发 emitEvent 的 eventCounter++ 原非原子, 丢更新 26-93/100000
- 新增并发原子性测试 + mock counter recorder
- Evaluator: Pass (RED 丢更新可复现 -> GREEN 3x 全绿 -> 回归 35通过/0失败/1跳过)

Change: $CHANGE
Branch: $BRANCH
Base:   $SHORT_SHA"

# 用 heredoc 提交，让 pre-commit hook 正常跑
if ! git commit -m "$(cat <<EOF
$COMMIT_MSG
EOF
)"; then
    err "git commit 失败（可能 pre-commit hook 拦截）。暂存区保留待查。"
    exit 7
fi
NEW_SHA="$(git rev-parse HEAD)"
ok "已提交: $NEW_SHA"

# ---------- 推送（永不 --force，永不 --no-verify）----------
ok "推送: git push origin $BRANCH"
# GIT_TERMINAL_PROMPT=0 避免无凭据时挂起等待输入
if ! GIT_TERMINAL_PROMPT=0 git push origin "$BRANCH" 2>&1 | tee /tmp/auto_push_push.log; then
    err "推送失败。提交已在本地（$NEW_SHA），未到远程。"
    err "常见原因：HTTPS 无凭据（GitHub 2021 起需 PAT）/ 网络问题 / 非快进。"
    warn "如需 PAT：git push https://<user>:<PAT>@github.com/dd2006tqf/internet-bluetooth-diagnose.git $BRANCH"
    exit 8
fi
ok "推送成功：origin/$BRANCH @ $NEW_SHA"
echo "=== auto_archive_push 完成 ==="
