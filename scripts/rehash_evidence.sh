#!/usr/bin/env bash
set -euo pipefail

# 级联哈希更新工具
# 当 spec/design/tasks 文件在证据收集后被修改时，fingerprint 会变化，
# 导致证据文件中的指纹过时。本工具自动更新所有证据文件和引用。
#
# 用法: scripts/rehash_evidence.sh [<change-name>]
#   不提供 change-name 时自动检测活跃 change

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

# 解析 change 名称
if [ $# -gt 0 ]; then
    CHANGE_NAME="$1"
elif [ -f ".ai-harness/active_change" ]; then
    CHANGE_NAME=$(cat .ai-harness/active_change)
else
    ACTIVE_CHANGES=$(find openspec/changes -maxdepth 1 -mindepth 1 -type d ! -name "archive" ! -name "stale" -exec basename {} \; 2>/dev/null || true)
    CHANGE_COUNT=$(echo "$ACTIVE_CHANGES" | grep -c . 2>/dev/null || echo 0)
    if [ "$CHANGE_COUNT" -eq 1 ]; then
        CHANGE_NAME="$ACTIVE_CHANGES"
    else
        echo "用法: $0 <change-name>" >&2
        echo "  自动检测失败（找到 $CHANGE_COUNT 个活跃 change）" >&2
        exit 1
    fi
fi

echo "级联哈希更新: $CHANGE_NAME"
node scripts/rehash_evidence.js "$CHANGE_NAME"
