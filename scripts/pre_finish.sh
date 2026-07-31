#!/usr/bin/env bash
# Pre-finish: 在 evaluator_check.sh --finish 之前运行
# 确保 footprint、integration report、evaluation.json 全部就绪
# 用法: scripts/pre_finish.sh <change>
set -euo pipefail

change=${1:?用法: pre_finish.sh <change>}
dir="openspec/changes/$change"
[[ -d "$dir" && ! -L "$dir" ]] || { echo "[ERR] change 不存在: $change" >&2; exit 2; }

echo "🔍 Pre-finish 检查: $change"
errors=0

# 1. Footprint
echo -n "  1. Footprint... "
if bash scripts/change_footprint.sh "$change" --json >/dev/null 2>&1; then
  status=$(node -p "JSON.parse(require('fs').readFileSync('$dir/harness/change-footprint.json')).status")
  echo "$status"
else
  echo "FAILED (重新生成中...)"
  bash scripts/change_footprint.sh "$change" --json >/dev/null 2>&1 || { echo "     [ERR] footprint 生成失败"; ((errors++)); }
fi

# 2. Integration surface report
echo -n "  2. Integration report... "
bash scripts/integration_surface_check.sh "$change" --refresh --json >/dev/null 2>&1 || { echo "FAILED"; ((errors++)); }
echo "OK"

# 3. Sync hashes
echo -n "  3. Hash sync... "
bash scripts/sync_hashes.sh "$change" >/dev/null 2>&1 || { echo "FAILED"; ((errors++)); }
echo "OK"

# 4. Evaluation JSON
eval_file="$dir/harness/evaluation.json"
if [[ -f "$eval_file" && ! -L "$eval_file" ]]; then
  echo -n "  4. evaluation.json... "
  bash scripts/evaluation_fix.sh "$change" >/dev/null 2>&1 || { echo "FAILED"; ((errors++)); }
  echo "OK"
else
  echo "  4. evaluation.json: 不存在 (先运行 evaluation_template.sh)"
fi

# 5. Final check
echo -n "  5. Integration check... "
bash scripts/integration_surface_check.sh "$change" --check --json >/dev/null 2>&1 && echo "OK" || { echo "BLOCKED"; ((errors++)); }

echo ""
if [[ $errors -eq 0 ]]; then
  echo "✅ 全部就绪，可以运行: scripts/evaluator_check.sh --finish"
  exit 0
else
  echo "❌ $errors 项检查失败，请修复后重试"
  exit 1
fi
