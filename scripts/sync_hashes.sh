#!/usr/bin/env bash
set -euo pipefail

# 自动同步 harness 中所有相关文件的哈希值
# 用于在修改 design.md 或其他关键文件后，快速更新所有级联哈希

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

# 优先使用命令行参数
if [ $# -gt 0 ]; then
    CHANGE_NAME="$1"
    echo "使用指定的 change: $CHANGE_NAME"
elif [ -f ".ai-harness/active_change" ]; then
    CHANGE_NAME=$(cat .ai-harness/active_change)
    echo "从 active_change 文件读取: $CHANGE_NAME"
else
    # 自动检测：查找 openspec/changes 中非 archive 的目录
    ACTIVE_CHANGES=$(find openspec/changes -maxdepth 1 -mindepth 1 -type d ! -name "archive" -exec basename {} \;)
    CHANGE_COUNT=$(echo "$ACTIVE_CHANGES" | wc -l)
    
    if [ "$CHANGE_COUNT" -eq 0 ]; then
        echo "错误: 没有活跃的 change"
        exit 1
    elif [ "$CHANGE_COUNT" -eq 1 ]; then
        CHANGE_NAME="$ACTIVE_CHANGES"
        echo "自动检测到活跃 change: $CHANGE_NAME"
    else
        echo "错误: 检测到多个活跃 change，请手动指定："
        echo "$ACTIVE_CHANGES" | sed 's/^/  - /'
        echo ""
        echo "用法: $0 <change-name>"
        exit 1
    fi
fi
CHANGE_DIR="openspec/changes/$CHANGE_NAME"
HARNESS_DIR="$CHANGE_DIR/harness"

if [ ! -d "$HARNESS_DIR" ]; then
    echo "错误: harness 目录不存在: $HARNESS_DIR"
    exit 1
fi

echo "开始同步 change: $CHANGE_NAME"
echo "================================"

# 运行 Node.js 脚本来同步哈希
node "$SCRIPT_DIR/sync_hashes.js" "$CHANGE_NAME"

echo "================================"
echo "哈希同步完成"
