#!/usr/bin/env bash
# ============================================================================
# WeakNet 边云协同智能网络诊断平台 — 一键拉起脚本
#
# 功能：
#   1. 检查并拉起中心云全套服务（PostgreSQL, Keycloak, Vault, Redis, OPA, Temporal, API, Web）
#   2. 自动检查 Windows portproxy 连通性
#   3. 可选同步编译并部署边缘开发板（C++ + eBPF）
#
# 用法:
#   ./start_platform.sh               # 仅拉起并检查中心云全套服务
#   ./start_platform.sh --deploy-edge # 拉起中心云 + 编译部署开发板
# ============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LMA_DIR="${ROOT}/Large-Model-Application"

GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
RED='\033[0;31m'
NC='\033[0m'

pass() { echo -e "${GREEN}✅ $1${NC}"; }
warn() { echo -e "${YELLOW}⚠️  $1${NC}"; }
info() { echo -e "${CYAN}ℹ️  $1${NC}"; }
fail() { echo -e "${RED}❌ $1${NC}"; exit 1; }

DEPLOY_EDGE=false
for arg in "$@"; do
  case "$arg" in
    --deploy-edge) DEPLOY_EDGE=true ;;
    --help|-h)
      echo "用法: $0 [--deploy-edge]"
      echo "  --deploy-edge   在拉起中心云后，自动调用 tools/ci.sh 增量编译并部署开发板"
      exit 0
      ;;
  esac
done

echo "================================================================="
echo "  WeakNet 边云协同智能网络诊断平台 — 一键启动程序"
echo "================================================================="

# 1. 检查 Docker 运行时
if ! command -v docker >/dev/null 2>&1; then
  fail "Docker 运行时未安装或不在 PATH 中"
fi

# 2. 初始化环境凭证
info "步骤 1/4: 初始化中心云环境配置..."
cd "$LMA_DIR"
if [ ! -f .env.m1.local ]; then
  ./scripts/dev_lite.sh init
  pass "已生成本地环境配置文件 .env.m1.local"
else
  info ".env.m1.local 已存在，沿用当前配置"
fi

# 3. 启动全套微服务
info "步骤 2/4: 启动 Docker Compose 微服务栈..."
M1_RUNTIME_ENV_FILE=.env.m1.local docker compose --env-file .env.m1.local -f compose.lite.yaml up -d

# 4. 等待后端健康就绪
info "步骤 3/4: 等待后端 API 健康检查就绪..."
ATTEMPTS=0
MAX_ATTEMPTS=30
while [ $ATTEMPTS -lt $MAX_ATTEMPTS ]; do
  if curl -s http://localhost:8000/health/ready | grep -q '"status":"ready"'; then
    pass "中心云微服务已全部健康就绪！"
    break
  fi
  ATTEMPTS=$((ATTEMPTS + 1))
  sleep 2
done

if [ $ATTEMPTS -eq $MAX_ATTEMPTS ]; then
  warn "后端 API 响应较慢，请稍后使用 'curl -s http://localhost:8000/health/ready' 确认状态"
fi

# 5. 提示或部署边缘端
if [ "$DEPLOY_EDGE" = true ]; then
  info "步骤 4/4: 增量编译并部署边缘开发板..."
  cd "$ROOT"
  ./tools/ci.sh --skip-test
  pass "开发板增量部署与重启完成！"
else
  info "步骤 4/4: 跳过开发板端部署（如需一键部署开发板，请附加 --deploy-edge 参数）"
fi

echo ""
echo "================================================================="
pass "WeakNet 智能网络诊断平台已拉起成功！"
echo ""
echo "  🌐 Web 控制台:          http://localhost:3000/network"
echo "  🤖 智能排障助手 (Copilot): http://localhost:3000/network/copilot"
echo "  📡 后端 API 健康就绪检查:  http://localhost:8000/health/ready"
echo "  📖 详细演示操作步骤文档:  PLATFORM_OPERATION_GUIDE.md"
echo "================================================================="
