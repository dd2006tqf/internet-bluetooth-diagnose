#!/usr/bin/env bash
# ============================================================================
# libcurl 私有 sysroot 安装脚本（ARM64 构建容器用）
# ============================================================================
#
# 为什么需要这个脚本
#
# 边缘遥测上报（W-edge）用 libcurl 发送 HTTP POST。ARM64 构建容器里
# libcurl4 **运行时库已装**（7.74.0-1.3+deb11u13），但**没有开发头文件**
# （/usr/include/curl/curl.h）与 libcurl.so 链接名，编译时会直接报
# "curl/curl.h: No such file or directory"。
#
# 为什么走"固定 .deb + SHA256 + dpkg-deb -x 私有 sysroot"，而不是 apt install
#
# 与 scripts/setup_openssl_sysroot.sh 同一套纪律：
#   1. apt 装的是 bullseye/main 当前索引，版本不可固定；snapshot.debian.org
#      的不可变时间戳快照才是可复现构建。
#   2. apt install 会改动容器运行时（libcurl4 已在镜像里，重新安装存在
#      版本漂移风险）；dpkg-deb -x 只解压到 /opt 私有路径，不污染容器。
#   3. SHA256 在调用方与固定常量逐一比对，完整性由哈希保证而非传输层。
#
# 版本选型
#
#   libcurl4-openssl-dev 7.74.0-1.3+deb11u13 —— 与镜像已装的 libcurl4
#   运行时（7.74.0-1.3+deb11u13）**完全同版本**。这保证：
#     - 产物 NEEDED 为 libcurl.so.4，与板端已有的
#       /usr/lib/aarch64-linux-gnu/libcurl.so.4.7.0（Debian 11.11）一致；
#     - 编译期头文件与运行期库 ABI 完全匹配，不引入符号缺失。
#
# ── 运行环境 ────────────────────────────────────────────────────────
# 必须在 ARM64 构建容器内执行（需要 dpkg-deb，且 sysroot 落在容器路径）：
#
#   docker exec weaknet-arm64-dev bash -c \
#     'cd /src && bash scripts/setup_libcurl_sysroot.sh'
#
# 产物（SYSROOT 下，Debian multiarch 布局）：
#   /opt/weaknet-libcurl/usr/include/aarch64-linux-gnu/curl/*.h   ← 头文件在此
#   /opt/weaknet-libcurl/usr/lib/aarch64-linux-gnu/libcurl.so    → libcurl.so.4（开发期链接）
#   /opt/weaknet-libcurl/usr/lib/aarch64-linux-gnu/libcurl.so.4
#   /opt/weaknet-libcurl/usr/lib/aarch64-linux-gnu/libcurl.so.4.7.0（从镜像运行时库补齐）
#
# ── 运行时依赖 ──────────────────────────────────────────────────────
# 产物 NEEDED 为 libcurl.so.4（依赖 libssl.so.1.1 / libcrypto.so.1.1），
# 与开发板已有的 libcurl4 / libssl1.1 一致。sysroot 只用于编译，不随包分发。
# ============================================================================

set -euo pipefail

SNAPSHOT_BASE="https://snapshot.debian.org/archive/debian/20260912T000000Z"
SYSROOT="${SYSROOT:-/opt/weaknet-libcurl}"

# 文件名|SHA256（均取自上述 snapshot 的 bullseye/main/binary-arm64 索引）
DEBS=(
  "libcurl4-openssl-dev_7.74.0-1.3+deb11u13_arm64.deb|5ee4867df77480fbe16220e0a6867d4f9e705491ccaa8ff9b11c22bae2161469"
)

# 判定 sysroot 是否已完整（缺任一关键文件即视为不完整）
#
# 注意 Debian multiarch 布局：curl 头文件在 usr/include/aarch64-linux-gnu/curl/
# 下（实测 -dev 包解压结果），usr/include/curl/ 不存在。CMake 侧对应地
# 需要同时加两条 include 路径（与 OpenSSL sysroot 的 opensslconf.h 处理一致）。
sysroot_complete() {
  local missing=() f
  for f in \
    "usr/include/aarch64-linux-gnu/curl/curl.h" \
    "usr/lib/aarch64-linux-gnu/libcurl.so.4" \
    "usr/lib/aarch64-linux-gnu/libcurl.so"
  do
    [ -e "${SYSROOT}/${f}" ] || missing+=("$f")
  done
  if [ ${#missing[@]} -gt 0 ]; then
    printf '    missing: %s\n' "${missing[@]}"
    return 1
  fi
  return 0
}

# 纯 perl HTTP 下载（容器内无 curl/wget 时的回退路径）。
# snapshot 的 pool 路径返回 302 → /file/<content-hash>/<name>，需手动跟随。
#
# 传输层说明：容器内 perl 无 IO::Socket::SSL，无法直接走 https，故此处把
# 调用方传入的 https:// 降级为 http:// 传输。这不是完整性降级 ——
# 下载结果的 SHA256 在调用方与固定常量逐一比对，完整性由哈希保证而非
# 传输层；且 snapshot.debian.org 的 /file/<content-hash>/ 路径本身即
# 内容寻址。有 curl/wget 的主机不会走到这条路径，仍使用完整 https。
perl_download() {
  perl -MIO::Socket::INET -e '
    my ($url, $out) = @ARGV;
    # 降级 https → http（见上方传输层说明）
    $url =~ s{^https://}{http://};
    my ($host, $path) = $url =~ m{^http://([^/]+)(/.*)$}
      or die "not an http url: $url\n";

    sub fetch {
      my ($h, $p) = @_;
      my $s = IO::Socket::INET->new(PeerHost => $h, PeerPort => 80, Timeout => 60)
        or die "connect $h: $!\n";
      print $s "GET $p HTTP/1.0\r\nHost: $h\r\n\r\n";
      binmode $s;
      my ($hdr, $c) = ("", "");
      while (read($s, $c, 1)) { $hdr .= $c; last if $hdr =~ /\r\n\r\n$/; }
      my ($st)  = $hdr =~ m{^HTTP/\S+\s+(\d+)};
      my ($loc) = $hdr =~ m{^location:\s*(\S+)}mi;
      return ($s, $st, $loc);
    }

    my ($s, $st, $loc) = fetch($host, $path);
    if ($st == 302 && defined $loc) {
      close($s);
      ($s, $st, $loc) = fetch($host, $loc);
    }
    die "http status $st for $path\n" unless $st == 200;

    open(my $fh, ">", $out) or die "open $out: $!\n";
    binmode $fh;
    my $buf;
    while (read($s, $buf, 65536)) { print $fh $buf; }
    close($fh);
  ' "$1" "$2"
}

download() {  # $1=url  $2=outfile
  if command -v curl >/dev/null 2>&1; then
    curl -fsSL --max-time 300 -o "$2" "$1"
  elif command -v wget >/dev/null 2>&1; then
    wget -q -O "$2" "$1"
  elif command -v perl >/dev/null 2>&1; then
    perl_download "$1" "$2"
  else
    echo "[!] 无可用下载工具（curl / wget / perl 均缺失）" >&2
    return 1
  fi
}

show_structure() {
  echo "[*] sysroot 结构："
  find "$SYSROOT" -maxdepth 5 \
    \( -name "curl.h" -o -name "libcurl.so*" \) 2>/dev/null | sort
}

echo "== libcurl private sysroot =="
echo "   snapshot: ${SNAPSHOT_BASE}"
echo "   sysroot : ${SYSROOT}"

if ! command -v dpkg-deb >/dev/null 2>&1; then
  echo "[!] dpkg-deb 不可用；本脚本需在 ARM64 构建容器内运行" >&2
  exit 1
fi

echo "[*] 检查既有 sysroot"
if sysroot_complete; then
  echo "[*] 已完整，跳过下载（幂等）"
  show_structure
  echo "[*] setup complete: ${SYSROOT}"
  exit 0
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "[*] 下载并校验 ${#DEBS[@]} 个 .deb"
for entry in "${DEBS[@]}"; do
  deb="${entry%%|*}"
  expect="${entry##*|}"
  url="${SNAPSHOT_BASE}/pool/main/c/curl/${deb}"

  echo "    - ${deb}"
  if ! download "$url" "${WORK}/${deb}"; then
    echo "[!] 下载失败: ${url}" >&2
    exit 1
  fi

  actual="$(sha256sum "${WORK}/${deb}" | awk '{print $1}')"
  if [ "$actual" != "$expect" ]; then
    echo "[!] SHA256 不匹配: ${deb}" >&2
    echo "      expect=${expect}" >&2
    echo "      actual=${actual}" >&2
    exit 1
  fi
  echo "      sha256 OK"
done

echo "[*] 解压到 ${SYSROOT}（dpkg-deb -x，不 install）"
mkdir -p "$SYSROOT"
for entry in "${DEBS[@]}"; do
  dpkg-deb -x "${WORK}/${entry%%|*}" "$SYSROOT"
done

# libcurl4-openssl-dev 的 libcurl.so → libcurl.so.4.7.0 是**悬空软链**：
# -dev 包只带开发符号链接，实际的 .so.4/.so.4.7.0 运行时文件由 libcurl4
# 提供（镜像里 /usr/lib/aarch64-linux-gnu/libcurl.so.4.7.0 已存在）。
# 从镜像的运行时库补齐软链目标，使 sysroot 内自洽、可独立链接。
# 这不改变运行时行为：产物 NEEDED 仍是 libcurl.so.4，运行时走系统库。
if [ ! -e "${SYSROOT}/usr/lib/aarch64-linux-gnu/libcurl.so.4.7.0" ] && \
   [ -e /usr/lib/aarch64-linux-gnu/libcurl.so.4.7.0 ]; then
  echo "[*] 从镜像运行时库补齐 libcurl.so.4 / libcurl.so.4.7.0（-dev 包不含运行时文件）"
  cp /usr/lib/aarch64-linux-gnu/libcurl.so.4.7.0 \
     "${SYSROOT}/usr/lib/aarch64-linux-gnu/libcurl.so.4.7.0"
  ln -sf libcurl.so.4.7.0 "${SYSROOT}/usr/lib/aarch64-linux-gnu/libcurl.so.4"
fi

echo "[*] 校验关键文件"
if ! sysroot_complete; then
  echo "[!] 解压后 sysroot 仍不完整" >&2
  exit 1
fi
echo "    关键文件齐全"

show_structure
echo "[*] setup complete: ${SYSROOT}"
