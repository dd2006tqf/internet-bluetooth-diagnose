#!/usr/bin/env bash
# change_abort.sh — 逃生舱：优雅中止变更，保留代码改动，清理 harness 产物
# 用法: bash scripts/change_abort.sh <change-name> [--purge]
#   --purge  同时删除 change 目录（默认只清理 harness 产物，保留规划文档）
set -euo pipefail
CHANGE="${1:-}"
PURGE=false
[ "${2:-}" = "--purge" ] && PURGE=true
[ -z "$CHANGE" ] && { echo "用法: $0 <change-name> [--purge]" >&2; exit 1; }
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BASE="$ROOT/openspec/changes/$CHANGE"
[ -d "$BASE" ] || { echo "[ERR] 变更目录不存在: $BASE" >&2; exit 1; }
HARNESS="$BASE/harness"

echo "========================================"
echo "  中止变更: $CHANGE"
echo "========================================"
echo ""

# 1. 列出将保留的代码改动（工作区源码不动）
echo "[1/4] 代码改动保留在工作区（不受影响）"
git status --short 2>/dev/null | head -10 || echo "  (git 不可用，跳过)"
echo ""

# 2. 清理 harness 产物
echo "[2/4] 清理 harness 产物..."
if [ -d "$HARNESS" ]; then
  rm -rf "$HARNESS/project-command-evidence"
  rm -f "$HARNESS/change-footprint.json"
  rm -f "$HARNESS/verification.json"
  rm -f "$HARNESS/verification.md"
  rm -f "$HARNESS/ai_snapshot.json"
  rm -f "$HARNESS/audit-summary.md"
  echo "  已删除: project-command-evidence/, change-footprint.json, verification.json, verification.md, ai_snapshot.json"
fi
echo ""

# 3. 清理 selector（如果指向此 change）
SELECTOR="$ROOT/.ai-harness/active-change.txt"
if [ -f "$SELECTOR" ] && grep -q "$CHANGE" "$SELECTOR" 2>/dev/null; then
  rm -f "$SELECTOR"
  echo "[3/4] 已清理 active-change selector"
else
  echo "[3/4] selector 无需清理"
fi
echo ""

# 4. 可选：删除整个 change 目录
if [ "$PURGE" = true ]; then
  rm -rf "$BASE"
  echo "[4/4] 已删除 change 目录: $BASE"
else
  echo "[4/4] 保留规划文档（proposal/design/tasks/specs）"
  echo "  如需彻底删除: bash scripts/change_abort.sh $CHANGE --purge"
fi
echo ""
echo "========================================"
echo "  中止完成"
echo "  注意: 工作区的源码改动仍然保留，"
echo "  你可以手动 git commit 或 git checkout"
echo "========================================"
