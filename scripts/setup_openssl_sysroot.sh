#!/usr/bin/env bash
#
# OpenSSL 1.1 私有 sysroot 建立（W1 门禁）
#
# ── 为什么不能走 APT ────────────────────────────────────────────────
# 实测（2026-09-13，干净 bullseye 容器，分别验证三条路）：
#   1. 默认 sources.list + bullseye-security
#      → 候选 libssl-dev 1.1.1w-0+deb11u8，但 snapshot pool 中该 .deb
#        文件缺失，恒 404。
#   2. snapshot 的 debian-security 归档
#      → 同样 404（索引里有、文件不在）。
#   3. snapshot 的 debian bullseye/main
#      → 只有 1.1.1w-0+deb11u1，而镜像已装的 libssl1.1 是 deb11u8，
#        apt 报 "Depends: libssl1.1 (= 1.1.1w-0+deb11u1) but
#        1.1.1w-0+deb11u8 is to be installed" → held broken packages。
#
# 因此改为：固定版本 .deb + 固定 SHA256 校验 + dpkg-deb -x 解压到私有
# sysroot。extract-only 不 install，APT 不参与依赖解析，不存在 libc6
# 版本冲突，也不污染容器运行时（镜像自带的 libssl.so.1.1 保持不动）。
#
# ── 为什么 SHA256 可以硬编码 ────────────────────────────────────────
# 下载地址固定在 snapshot.debian.org 的不可变时间戳快照
# （20260912T000000Z）。该快照的 bullseye/main/binary-arm64 Packages
# 索引记录的 SHA256 即为固定值，下方常量取自该索引的 SHA256 字段，
# 并已实际下载复核一致。snapshot URL 不可变 → 校验值不可变。
#
# 注意不能像早期版本那样运行时从 Packages.gz 反查：bullseye 的
# Packages.gz 有 10.9 MB，且反查逻辑依赖 Filename 前缀匹配，实测返回
# 空值导致校验必然失败。固定常量既正确又免去 11 MB 下载。
#
# ── 运行位置 ────────────────────────────────────────────────────────
# 必须在 ARM64 构建容器内执行（需要 dpkg-deb，且 sysroot 落在容器路径）：
#
#   docker exec weaknet-arm64-dev bash -c \
#     'cd /src && bash scripts/setup_openssl_sysroot.sh'
#
# 容器内无 curl/wget/python3，只有 perl，因此下载按
# curl → wget → perl(IO::Socket::INET) 顺序回退，三条路都已验证可用
# （perl 路线在容器内实测下载并 SHA256 校验通过）。
#
# ── 幂等性 ──────────────────────────────────────────────────────────
# 关键文件齐全时直接跳过并打印结构，不重新下载、不覆盖既有内容。
#
# ── 产出结构 ────────────────────────────────────────────────────────
#   $SYSROOT/usr/include/openssl/*.h
#   $SYSROOT/usr/include/aarch64-linux-gnu/openssl/opensslconf.h   ← 必需
#   $SYSROOT/usr/lib/aarch64-linux-gnu/libssl.so.1.1
#   $SYSROOT/usr/lib/aarch64-linux-gnu/libcrypto.so.1.1
#   $SYSROOT/usr/lib/aarch64-linux-gnu/libssl.so → libssl.so.1.1（开发期链接）
#
# CMake 必须**同时**把 usr/include 与 usr/include/aarch64-linux-gnu 加入
# include 路径：opensslconf.h 位于后者（Debian 的 multiarch 布局），
# 只给前者会报 "openssl/opensslconf.h: No such file or directory"。
#
# ── 运行时依赖 ──────────────────────────────────────────────────────
# 产物 NEEDED 为 libssl.so.1.1 / libcrypto.so.1.1，与开发板已有的
# libssl1.1 一致（板端实测 1.1.1w-0+deb11u5）。sysroot 只用于编译，
# 不随包分发。

set -euo pipefail

SNAPSHOT_BASE="https://snapshot.debian.org/archive/debian/20260912T000000Z"
SYSROOT="${SYSROOT:-/opt/weaknet-openssl-1.1}"

# 文件名|SHA256（均取自上述 snapshot 的 bullseye/main/binary-arm64 索引）
DEBS=(
  "libssl1.1_1.1.1w-0+deb11u1_arm64.deb|fe7a7d313c87e46e62e614a07137e4a476a79fc9e5aab7b23e8235211280fee3"
  "libssl-dev_1.1.1w-0+deb11u1_arm64.deb|6223f761bd4b961aa0c7c1662a6e99ab31150a1a7e6529d440428b361a9c8c4a"
)

# 判定 sysroot 是否已完整（缺任一关键文件即视为不完整）
sysroot_complete() {
  local missing=() f
  for f in \
    "usr/include/openssl/ssl.h" \
    "usr/include/aarch64-linux-gnu/openssl/opensslconf.h" \
    "usr/lib/aarch64-linux-gnu/libssl.so.1.1" \
    "usr/lib/aarch64-linux-gnu/libcrypto.so.1.1"
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
    \( -name "ssl.h" -o -name "opensslconf.h" \
       -o -name "libssl.so*" -o -name "libcrypto.so*" \) 2>/dev/null | sort
}

echo "== OpenSSL 1.1 private sysroot =="
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
  url="${SNAPSHOT_BASE}/pool/main/o/openssl/${deb}"

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

echo "[*] 校验关键文件"
if ! sysroot_complete; then
  echo "[!] 解压后 sysroot 仍不完整" >&2
  exit 1
fi
echo "    关键文件齐全"

show_structure
echo "[*] setup complete: ${SYSROOT}"
