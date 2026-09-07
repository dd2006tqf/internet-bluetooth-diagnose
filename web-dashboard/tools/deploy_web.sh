#!/usr/bin/env bash
# =============================================================================
# deploy_web.sh — 一键将 WeakNet Web 可视化仪表盘部署到开发板并配置 systemd
# =============================================================================

set -euo pipefail

BOARD="${BOARD:-radxa@192.168.137.210}"
REMOTE_DIR="/home/radxa/weaknet/web"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WEB_DASHBOARD_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "================================================"
echo "  Deploying WeakNet Web Dashboard to $BOARD"
echo "================================================"

echo "--- 1. 检查开发板连通性 ---"
if ! ping -c 1 -W 2 "${BOARD#*@}" >/dev/null 2>&1; then
    echo "❌ 无法连接到开发板: ${BOARD#*@}"
    exit 1
fi
echo "✅ 开发板在线"

echo "--- 2. 准备远端目录与同步文件 ---"
ssh "$BOARD" "mkdir -p $REMOTE_DIR/backend $REMOTE_DIR/frontend $REMOTE_DIR/AI-assisted-analysis"

# 同步后端、前端和 AI 知识库依赖
rsync -az --delete \
    "$WEB_DASHBOARD_DIR/backend/" \
    "$BOARD:$REMOTE_DIR/backend/"

rsync -az --delete \
    "$WEB_DASHBOARD_DIR/frontend/" \
    "$BOARD:$REMOTE_DIR/frontend/"

# 同步 AI 知识库源码（用于 RAG 规则）
rsync -az \
    "$WEB_DASHBOARD_DIR/../AI-assisted analysis/network_knowledge_base.py" \
    "$BOARD:$REMOTE_DIR/backend/"

echo "✅ 文件同步完成"

echo "--- 3. 安装开发板 Python Web 依赖 (fastapi, uvicorn) ---"
ssh "$BOARD" << 'EOF'
if ! python3 -c "import fastapi, uvicorn" 2>/dev/null; then
    echo "正在安装 FastAPI 与 Uvicorn..."
    pip3 install -q fastapi uvicorn websockets || sudo apt-get install -y -qq python3-fastapi python3-uvicorn 2>/dev/null || pip3 install fastapi uvicorn websockets --break-system-packages 2>/dev/null
fi
echo "✅ Python 依赖就绪"
EOF

echo "--- 4. 确保历史数据目录只读权限并启动 systemd 服务 ---"
ssh "$BOARD" << 'EOF'
sudo chmod o+rx /home/radxa/weaknet/data 2>/dev/null || true
sudo cp /home/radxa/weaknet/web/backend/weaknet-web.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable weaknet-web.service
sudo systemctl restart weaknet-web.service
sleep 2
sudo systemctl is-active --quiet weaknet-web.service && echo "✅ weaknet-web 服务运行正常" || {
    echo "❌ weaknet-web 服务启动异常，日志如下:"
    sudo journalctl -u weaknet-web.service -n 20 --no-pager
    exit 1
}
EOF

echo "================================================"
echo "🎉 Web 可视化仪表盘部署成功！"
echo ""
echo "浏览器访问指南："
echo "  [方式 1: 局域网直连 (推荐)]"
echo "    http://${BOARD#*@}:8080"
echo "    http://radxa-cubie-a7a.local:8080"
echo ""
echo "  [方式 2: SSH 反向隧道 (远程/异地)]"
echo "    ssh -L 8080:127.0.0.1:8080 $BOARD"
echo "    然后浏览器打开: http://localhost:8080"
echo "================================================"
