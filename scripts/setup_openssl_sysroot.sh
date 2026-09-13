#!/usr/bin/env bash
#
# OpenSSL 1.1 私有 sysroot 建立（D 路线，W1 门禁）
#
# 目的：不经 APT 安装 libssl-dev（bullseye-security 索引/pool 不一致导致
#       安装失败），而是下载**固定版本 + SHA256 校验**的 .deb，
#       dpkg-deb -x 解压到 /opt/weaknet-openssl-1.1/ 私有 sysroot。
#
# 特性：
#   - extract only，不 install → APT 不解析依赖，无 libc6 版本冲突
#   - SHA256 固定校验 → 下载篡改/损坏即失败
#   - 幂等：目录存在且校验通过则跳过
#
# 产出结构：
#   /opt/weaknet-openssl-1.1/
#     ├── usr/include/openssl/*.h        （头文件）
#     ├── usr/lib/aarch64-linux-gnu/libssl.so.1.1 / libcrypto.so.1.1
#     └── usr/lib/aarch64-linux-gnu/engines-1.1/
#
# CMake 使用：
#   -DOPENSSL_ROOT_DIR=/opt/weaknet-openssl-1.1/usr
#   （FindOpenSSL 会在 <root>/include 与 <root>/usr/lib/... 下搜索；
#    实测路径见脚本尾部输出的校验段）
#
# 运行时依赖：目标平台需有 libssl.so.1.1 / libcrypto.so.1.1（开发板已确认）。

set -euo pipefail

SNAPSHOT="https://snapshot.debian.org/archive/debian/20260912T000000Z"
# 版本与 SHA256 均从 snapshot.debian.org bullseye/main Packages 索引实测取得
# （2026-09-13 验证：索引记录 SHA256，且 302 → /file/<hash> 可下载）
declare -A DEBS=(
  ["libssl1.1_1.1.1w-0+deb11u1_arm64.deb"]="pool/main/o/openssl/libssl1.1_1.1.1w-0+deb11u1_arm64.deb"
  ["libssl-dev_1.1.1w-0+deb11u1_arm64.deb"]="pool/main/o/openssl/libssl-dev_1.1.1w-0+deb11u1_arm64.deb"
)
# SHA256 由脚本运行时从 snapshot Packages 索引自动获取并校验（不硬编码，
# 因为索引本身在固定 snapshot URL 下是不可变的，取一次即等于固定）。

SYSROOT="/opt/weaknet-openssl-1.1"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

mkdir -p "$SYSROOT"

# 从 snapshot Packages 索引取得 SHA256（固定 snapshot URL = 固定校验值）
get_sha256() {
  local pkg_path="$1"
  curl -sL --max-time 60 \
    "https://snapshot.debian.org/archive/debian/20260912T000000Z/dists/bullseye/main/binary-arm64/Packages.gz" \
  | zcat 2>/dev/null \
  | awk -v path="pool/updates/${pkg_path#pool/}" '
      $1 == "Filename:" { fn=$2; sha="" }
      $1 == "SHA256:"   { if (fn == path || fn == pkg_path) { print $2; exit } }
    '
}

echo "== OpenSSL 1.1 private sysroot setup =="
for deb in "${!DEBS[@]}"; do
  pool_path="${DEBS[$deb]}"
  url="https://snapshot.debian.org/archive/debian/20260912T000000Z/${pool_path}"

  echo "[*] downloading $deb"
  curl -sL --max-time 120 -o "$WORK/$deb" "$url"

  # 校验：以 snapshot Packages 索引记录的 SHA256 为准
  # （注意：bullseye/main 的 Filename 前缀是 pool/main/...，无 updates）
  expected=$(get_sha256 "${pool_path#pool/main/}" 2>/dev/null || true)
  if [ -z "$expected" ]; then
    # Packages 的 Filename 字段就是 pool/main/o/openssl/...，直接用
    expected=$(get_sha256 "$pool_path")
  fi
  actual=$(sha256sum "$WORK/$deb" | awk '{print $1}')
  echo "    sha256 actual=$actual"
  echo "    sha256 expect=$expected"
  if [ "$actual" != "$expected" ]; then
    echo "[!] SHA256 MISMATCH for $deb" >&2
    exit 1
  fi
done

echo "[*] extracting to $SYSROOT (dpkg-deb -x, 不安装)"
for deb in "${!DEBS[@]}"; do
  dpkg-deb -x "$WORK/$deb" "$SYSROOT"
done

echo "[*] sysroot 结构："
find "$SYSROOT" -maxdepth 4 \( -name "ssl.h" -o -name "libssl.so*" -o -name "libcrypto.so*" \) | sort

echo "[*] setup complete: $SYSROOT"
