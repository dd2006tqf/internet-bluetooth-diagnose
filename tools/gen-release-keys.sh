#!/usr/bin/env bash
# ============================================================================
# gen-release-keys.sh — 生成 ECDSA P-256 发布签名密钥对（OTA Phase 2）
#
# 选型说明：
#   采用 ECDSA NIST P-256 (prime256v1) + SHA256。
#   完美兼容开发板 Debian Bullseye 自带的 OpenSSL 1.1.1 与宿主机 OpenSSL 3.x CLI。
#
# 输出：
#   secrets/release_priv.pem (0600, 私钥，严禁提交到 git)
#   certs/release_pub.pem    (0644, 公钥，随代码仓库和镜像发布)
# ============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SECRETS_DIR="${ROOT}/secrets"
CERTS_DIR="${ROOT}/certs"
PRIV_KEY="${SECRETS_DIR}/release_priv.pem"
PUB_KEY="${CERTS_DIR}/release_pub.pem"

FORCE=false
if [ "${1:-}" = "--force" ] || [ "${1:-}" = "-f" ]; then
    FORCE=true
fi

mkdir -p "${SECRETS_DIR}" "${CERTS_DIR}"

if [ -f "${PRIV_KEY}" ] && [ "$FORCE" = false ]; then
    echo "⚠️  私钥已存在：${PRIV_KEY}"
    echo "如需重新生成，请使用 --force 参数（注意：重新生成后，旧版本升级包验签将失效！）"
    exit 0
fi

echo "生成 ECDSA prime256v1 (P-256) 发布私钥..."
openssl ecparam -name prime256v1 -genkey -noout -out "${PRIV_KEY}"
chmod 0600 "${PRIV_KEY}"

echo "提取 ECDSA 发布公钥..."
openssl ec -in "${PRIV_KEY}" -pubout -out "${PUB_KEY}" 2>/dev/null
chmod 0644 "${PUB_KEY}"

echo "✅ 密钥对生成成功！"
echo "  私钥 (仅本地/CI 保密): ${PRIV_KEY}"
echo "  公钥 (公开，分发至设备): ${PUB_KEY}"
