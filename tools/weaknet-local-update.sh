#!/usr/bin/env bash
# ============================================================================
# weaknet-local-update.sh — OTA Phase 1 板端本地升级执行体
#
# 职责：解包校验 → 版本单调检查 → 原子切换 current 软链接 → 重启服务 →
#       健康窗口 → 转正（.committed）或自动回滚上一版本。
#
# 用法：
#   weaknet-local-update.sh status
#   weaknet-local-update.sh verify <pkg.tar.zst|release-dir>
#   weaknet-local-update.sh apply <pkg.tar.zst> [--allow-downgrade]
#                                    [--commit]        # = --no-health，跳过健康窗口直接转正
#                                    [--no-health] [--health-timeout N]
#   weaknet-local-update.sh rollback [--to <version>] [--health-timeout N]
#   weaknet-local-update.sh adopt <staged-dir> <version> [--no-health]
#   weaknet-local-update.sh package <dist-dir> <version> <out.tar.zst>
#
# 环境变量：
#   WEAKNET_ROOT              安装根（默认 /home/radxa/weaknet）
#   WEAKNET_UPDATE_DRYRUN=1   跳过 systemctl/busctl（本地沙盒演练用）
#   WEAKNET_RELEASE_PRIV      发布私钥 PEM（package 时签名；不设则不签）
#   WEAKNET_RELEASE_PUB       板端验签公钥（apply 时验签；不设且未加 --insecure 则拒绝）
#
# 板端布局（升级建立后）：
#   $ROOT/releases/<version>/{server,client,lib,...,VERSION,manifest.sha256,.committed}
#   $ROOT/current -> releases/<version>          # 原子翻转点
#   $ROOT/{server,client,lib,...} -> current/*   # 兼容软链，历史路径不变
#   $ROOT/data/ 与 $ROOT/config/ 升级永不触碰
#
# 设计约束：
#   - apply/rollback 会重启 weaknet-server，必须 root 执行（status/verify/package 不需要）
#   - data/ 下 5 个状态文件（history.db / network-epoch / wireless-uplink-state /
#     runtime-overrides.bin / config_txn_state）与 /etc/weaknet/config.yaml 全程只读
#   - 版本比较用 dpkg --compare-versions（Debian 基线自带，数字感知）
#   - 改了 ActionRegistry/白名单的版本，发布前必须同步云端目录评审
#     （docs/网关动作目录版本契约.md，fail-closed 防线）
# ============================================================================
set -euo pipefail

ROOT="${WEAKNET_ROOT:-/home/radxa/weaknet}"
RELEASES="$ROOT/releases"
CURRENT_LINK="$ROOT/current"
SERVICE="weaknet-server"
BUS_DEST="com.example.WeakNet"
BUS_PATH="/com/example/WeakNet"
BUS_IFACE="com.example.WeakNet"
HEALTH_POLL_S="${HEALTH_POLL_S:-5}"
HEALTH_OK_NEEDED=3
COMPAT_ENTRIES=(server client lib edge_network_rag.py network_knowledge_base.py)

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
# 诊断信息一律走 stderr：stdout 只留可被命令替换捕获的数据（版本号/目录路径/JSON）
pass()  { echo -e "${GREEN}✅ $1${NC}" >&2; }
fail()  { echo -e "${RED}❌ $1${NC}" >&2; exit 1; }
info()  { echo -e "${CYAN}ℹ️  $1${NC}" >&2; }
warn()  { echo -e "${YELLOW}⚠️  $1${NC}" >&2; }

DRYRUN="${WEAKNET_UPDATE_DRYRUN:-0}"
require_root() {
    [ "$DRYRUN" = "1" ] && return 0
    [ "$(id -u)" = "0" ] || fail "此命令需要 root 权限（sudo 执行）"
}

# ---- 通用工具 ---------------------------------------------------------------

current_version() {
    # current -> releases/<ver>；无链接/非法版本名返回空
    local t
    t="$(readlink "$CURRENT_LINK" 2>/dev/null || true)"
    [ -z "$t" ] && return 0
    t="${t##*/}"
    if [[ "$t" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
        echo "$t"
    fi
}

valid_version() { [[ "$1" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; }

# 版本策略：target >= current（数字比较）；current 为空 = 首次安装放行
version_allowed() {  # $1=current $2=target $3=allow_downgrade(0/1)
    local cur="$1" tgt="$2" allow="${3:-0}"
    valid_version "$tgt" || return 1
    [ -z "$cur" ] && return 0
    valid_version "$cur" || return 1
    dpkg --compare-versions "$tgt" ge "$cur" && return 0
    [ "$allow" = "1" ] && return 0
    return 1
}

atomic_symlink() {  # $1=linkname(相对 releases/) $2=linkpath(绝对)
    ln -sfn "$1" "$2.tmp.$$"
    mv -Tf "$2.tmp.$$" "$2"
}

# current 翻转后维护兼容软链：$ROOT/{server,client,...} -> current/<name>
refresh_compat_links() {
    local e
    for e in "${COMPAT_ENTRIES[@]}"; do
        if [ -e "$CURRENT_LINK/$e" ] || [ -L "$CURRENT_LINK/$e" ]; then
            # 目标真实存在才建链；已有同名真目录（旧扁平布局残留）时跳过并告警
            if [ -e "$ROOT/$e" ] && [ ! -L "$ROOT/$e" ]; then
                warn "跳过兼容软链 $e：$ROOT/$e 是真实目录（旧布局残留），请人工确认"
                continue
            fi
            atomic_symlink "current/$e" "$ROOT/$e"
        fi
    done
}

svc_cmd() {  # systemctl 包装（DRYRUN 下视为成功）
    if [ "$DRYRUN" = "1" ]; then return 0; fi
    systemctl "$@"
}

bus_call() {  # busctl 只读调用包装；返回 0=成功
    [ "$DRYRUN" = "1" ] && return 0
    busctl --system call "$BUS_DEST" "$BUS_PATH" "$BUS_IFACE" "$1" >/dev/null 2>&1
}

# 健康窗口：服务 active + HealthCheck/GetEbpfMonitorHealth 可达 +
# GetUpdateStatus 报告的 installed_version 等于期望版本，连续
# HEALTH_OK_NEEDED 次全过才算健康；总超时 $1（秒）。$2=期望版本（可空）
health_window() {
    local timeout="${1:-120}" expect_ver="${2:-}"
    [ "$DRYRUN" = "1" ] && return 0
    local deadline=$(( $(date +%s) + timeout ))
    local streak=0
    while [ "$(date +%s)" -lt "$deadline" ]; do
        local ok=0
        if [ "$(systemctl is-active "$SERVICE" 2>/dev/null || true)" = "active" ] \
           && bus_call HealthCheck \
           && bus_call GetEbpfMonitorHealth; then
            # GetUpdateStatus 必须报告目标版本已就位（防"健康但跑在旧版"假绿灯）
            local st
            st="$(busctl --system call "$BUS_DEST" "$BUS_PATH" "$BUS_IFACE" GetUpdateStatus 2>/dev/null || true)"
            st="${st//\\/}"
            if [ -z "$expect_ver" ] || grep -q "\"installed_version\":\"$expect_ver\"" <<<"$st"; then
                ok=1
            fi
        fi
        if [ "$ok" = "1" ]; then
            streak=$((streak + 1))
            if [ "$streak" -ge "$HEALTH_OK_NEEDED" ]; then
                pass "健康窗口通过（连续 ${streak} 次探针全绿）"
                return 0
            fi
        else
            streak=0
        fi
        sleep "$HEALTH_POLL_S"
    done
    return 1
}

# 翻转 + 重启 + 健康 + 转正/回滚。
# $1=target版本 $2=prev版本(可空) $3=health_timeout $4=同版本旧副本目录(可空，失败时恢复)
# 成功退出 0；健康失败回滚成功退出 2；回滚失败退出 3
flip_restart_commit() {
    local target="$1" prev="${2:-}" htimeout="${3:-120}" old_dir="${4:-}"

    atomic_symlink "releases/$target" "$CURRENT_LINK"
    refresh_compat_links
    svc_cmd daemon-reload || true
    svc_cmd restart "$SERVICE" || warn "restart 返回非零（继续观察健康窗口）"

    if health_window "$htimeout" "$target"; then
        touch "$RELEASES/$target/.committed"
        # 转正后才清理同版本旧副本（.$ver.old.*）；其他历史版本保留为回滚目标
        rm -rf "$RELEASES"/."$target".old.* 2>/dev/null || true
        pass "升级完成并转正：releases/${target}"
        return 0
    fi

    warn "健康窗口未通过，自动回滚"
    # 同版本重装失败：先恢复旧副本内容，再把 current 指回去
    if [ -n "$old_dir" ] && [ -d "$old_dir" ]; then
        rm -rf "$RELEASES/$target"
        mv "$old_dir" "$RELEASES/$target"
        warn "已恢复同版本旧副本内容"
    fi
    if [ -n "$prev" ] && [ -d "$RELEASES/$prev" ]; then
        atomic_symlink "releases/$prev" "$CURRENT_LINK"
        refresh_compat_links
        svc_cmd restart "$SERVICE" || true
        if health_window 60 "$prev"; then
            fail "新版本 ${target} 健康失败，已回滚到 ${prev}（exit 2，新版本未转正）"
        fi
        fail "新版本 ${target} 健康失败且回滚后 ${prev} 也不健康（exit 3，需人工介入）"
    fi
    fail "新版本 ${target} 健康失败且无可回滚的上一版本（exit 3，需人工介入）"
}

# 校验一个 release 目录：manifest 逐文件 sha256 + VERSION 合法。$1=dir
# 成功时 stdout 只输出版本号（诊断信息走 stderr）；失败直接 fail 退出
verify_release_dir() {
    local dir="$1"
    [ -f "$dir/VERSION" ] || fail "release 缺少 VERSION 文件：$dir"
    local ver; ver="$(tr -d '[:space:]' < "$dir/VERSION")"
    valid_version "$ver" || fail "VERSION 内容非法：$ver"
    if [ -f "$dir/manifest.sha256" ]; then
        # sha256sum -c 的 "file: FAILED" 行走 stdout，必须丢弃——否则会被
        # 命令替换方捕获进版本变量（实测踩过：ver 变成 "xxx: FAILED"）
        (cd "$dir" && sha256sum --quiet -c manifest.sha256 >/dev/null 2>&1) \
            || fail "manifest 完整性校验失败：$dir"
        info "manifest.sha256 全量校验通过（$(grep -c '^[0-9a-f]' "$dir/manifest.sha256" || true) 个文件）"
    else
        warn "release 无 manifest.sha256（开发机 adopt 场景可接受；正式包必须携带）"
    fi
    echo "$ver"
}

# 签名验证（Phase 2 生产级强制验签）：
#   默认 fail-closed：必须存在发布公钥且签名有效
#   仅当显式指定 allow_unsigned=1 时告警放行
verify_signature() {  # $1=pkg 文件 $2=allow_unsigned(0/1)
    local pkg="$1" allow_unsigned="${2:-0}"
    local pub="${WEAKNET_RELEASE_PUB:-$ROOT/config/release_pub.pem}"
    if [ ! -f "$pub" ]; then
        if [ "$allow_unsigned" = "1" ]; then
            warn "未配置发布公钥（$pub），由于 --insecure/--allow-unsigned 标志，跳过验签"
            return 0
        else
            fail "板端未配置发布公钥（$pub），拒绝未授权升级（fail-closed；调试请加 --insecure）"
        fi
    fi
    [ -f "$pkg.sig" ] || fail "已配置发布公钥但缺少签名文件 $pkg.sig（拒绝未签名包）"
    openssl dgst -sha256 -verify "$pub" -signature "$pkg.sig" "$pkg" >/dev/null 2>&1 \
        || fail "数字签名校验失败：$pkg（密钥不匹配或包被篡改）"
    pass "ECDSA (P-256) 数字签名校验通过"
}

# 解包 + 校验 + 版本策略 → 输出 staging 目录路径。$1=pkg $2=allow_downgrade $3=allow_unsigned
stage_package() {
    local pkg="$1" allow="${2:-0}" allow_unsigned="${3:-0}"
    [ -f "$pkg" ] || fail "升级包不存在：$pkg"
    verify_signature "$pkg" "$allow_unsigned"
    local cur; cur="$(current_version)"
    local staging="$RELEASES/.staging.$$"
    rm -rf "$staging"; mkdir -p "$staging"
    case "$pkg" in
        *.tar.zst|*.tzst) tar --zstd -xf "$pkg" -C "$staging" ;;
        *.tar.gz|*.tgz)   tar -zxf "$pkg" -C "$staging" ;;
        *.tar)            tar -xf "$pkg" -C "$staging" ;;
        *) rm -rf "$staging"; fail "不支持的包格式：$pkg（需 tar.zst/tar.gz/tar）" ;;
    esac
    local ver
    # 显式接住校验失败：函数内的 fail 在命令替换子 shell 里只体现为非零退出码，
    # 不加 || 会穿透 set -e 继续执行（实测踩过）
    ver="$(verify_release_dir "$staging")" || { rm -rf "$staging"; exit 1; }
    # .committed 只能由健康窗口通过后 touch 生成：包内夹带（复制 release 打包、
    # 或手工构造）必须在落位前剥掉，否则新版本未过健康检查就已"转正"
    rm -f "$staging/.committed"
    if ! version_allowed "$cur" "$ver" "$allow"; then
        rm -rf "$staging"
        fail "版本策略拒绝：current=${cur:-无} → target=${ver}（降级需 --allow-downgrade）"
    fi
    echo "$staging"
}

install_self_script() {  # $1=新 release 目录：自更新脚本到稳定路径
    local rel="$1"
    if [ -f "$rel/weaknet-local-update.sh" ] && [ "$(readlink -f "$rel/weaknet-local-update.sh")" != "$(readlink -f "$0")" ]; then
        install -m 0755 "$rel/weaknet-local-update.sh" "$ROOT/weaknet-local-update.sh.new"
        mv -f "$ROOT/weaknet-local-update.sh.new" "$ROOT/weaknet-local-update.sh"
        info "升级脚本已自更新到 $ROOT/weaknet-local-update.sh"
    fi
}

# ---- 子命令 -----------------------------------------------------------------

cmd_status() {
    local cur; cur="$(current_version)"
    echo "root:            $ROOT"
    if [ -n "$cur" ]; then
        echo "layout:          releases"
        echo "installed:       $cur"
        echo "committed:       $([ -f "$RELEASES/$cur/.committed" ] && echo yes || echo no)"
    else
        echo "layout:          flat（未迁移，升级脚本首次 apply/adopt 后建立 releases 布局）"
        echo "installed:       unmanaged"
    fi
    local prev; prev="$(cat "$RELEASES/.previous" 2>/dev/null || true)"
    echo "previous:        ${prev:-无}"
    echo "releases:"
    local d
    for d in "$RELEASES"/*/; do
        [ -d "$d" ] || continue
        local v; v="$(basename "$d")"
        local mark=" "
        [ -e "$CURRENT_LINK" ] && [ "$(readlink "$CURRENT_LINK")" = "releases/$v" ] && mark="*"
        [ -f "$d/.committed" ] && mark="${mark}c" || mark="${mark} "
        echo "  $mark $v"
    done
}

cmd_verify() {  # $1=pkg|dir 其余为选项
    local target="$1"; shift || true
    local allow_unsigned=0
    while [ $# -gt 0 ]; do
        case "$1" in
            --insecure|--allow-unsigned) allow_unsigned=1 ;;
            *) fail "verify 未知参数：$1" ;;
        esac
        shift
    done
    [ -n "$target" ] || fail "用法：verify <pkg.tar.zst|release-dir> [--insecure]"
    if [ -d "$target" ]; then
        verify_release_dir "$target" >/dev/null
        pass "release 目录校验通过：$target"
    else
        local cur; cur="$(current_version)"
        local staging; staging="$(stage_package "$target" 0 "$allow_unsigned")" || exit 1
        local ver; ver="$(tr -d '[:space:]' < "$staging/VERSION")"
        rm -rf "$staging"
        pass "升级包校验通过：version=${ver}（current=${cur:-无}，策略允许）"
    fi
}

cmd_apply() {  # $1=pkg 其余为选项
    require_root
    local pkg="$1"; shift
    local allow=0 health=1 htimeout=120 allow_unsigned=0
    while [ $# -gt 0 ]; do
        case "$1" in
            --allow-downgrade) allow=1 ;;
            --commit|--no-health) health=0 ;;
            --health-timeout) htimeout="$2"; shift ;;
            --insecure|--allow-unsigned) allow_unsigned=1 ;;
            *) fail "apply 未知参数：$1" ;;
        esac
        shift
    done
    [ -n "$pkg" ] || fail "用法：apply <pkg.tar.zst> [--allow-downgrade] [--commit] [--health-timeout N] [--insecure]"

    local cur; cur="$(current_version)"
    info "current=${cur:-无（首次/未迁移）} → 应用 $pkg"
    local staging; staging="$(stage_package "$pkg" "$allow" "$allow_unsigned")" || exit 1
    local ver; ver="$(tr -d '[:space:]' < "$staging/VERSION")"

    svc_cmd stop "$SERVICE" 2>/dev/null || true

    # 记录回滚指针（首次安装无 prev，写空）
    if [ -n "$cur" ]; then echo "$cur" > "$RELEASES/.previous"; else rm -f "$RELEASES/.previous"; fi

    # 目标版本目录替换（同版本幂等重装）：旧副本保留到健康窗口通过才清理
    local old_dir=""
    if [ -d "$RELEASES/$ver" ]; then
        old_dir="$RELEASES/.$ver.old.$$"
        rm -rf "$old_dir"
        mv "$RELEASES/$ver" "$old_dir"
    fi
    mv "$staging" "$RELEASES/$ver"
    install_self_script "$RELEASES/$ver"

    if [ "$health" = "0" ]; then
        atomic_symlink "releases/$ver" "$CURRENT_LINK"
        refresh_compat_links
        svc_cmd daemon-reload || true
        svc_cmd restart "$SERVICE" || warn "restart 返回非零（--commit 模式不看健康窗口）"
        touch "$RELEASES/$ver/.committed"
        [ -n "$old_dir" ] && rm -rf "$old_dir"
        warn "--commit：跳过健康窗口，已直接转正 releases/${ver}"
        return 0
    fi

    flip_restart_commit "$ver" "$cur" "$htimeout" "$old_dir"
}

cmd_rollback() {  # [--to <version>]
    require_root
    local target="" htimeout=120
    while [ $# -gt 0 ]; do
        case "$1" in
            --to) target="$2"; shift ;;
            --health-timeout) htimeout="$2"; shift ;;
            *) fail "rollback 未知参数：$1" ;;
        esac
        shift
    done
    local cur; cur="$(current_version)"
    [ -n "$cur" ] || fail "当前无 releases 布局，无法回滚"
    if [ -z "$target" ]; then
        target="$(cat "$RELEASES/.previous" 2>/dev/null || true)"
        [ -n "$target" ] || fail "无 .previous 记录，无法自动回滚；可用 --to <version> 指定"
    fi
    valid_version "$target" || fail "回滚目标版本非法：$target"
    [ -d "$RELEASES/$target" ] || fail "回滚目标不存在：releases/$target"
    [ "$target" = "$cur" ] && fail "回滚目标与当前版本相同：$cur"

    info "回滚：${cur} → ${target}"
    echo "$cur" > "$RELEASES/.previous"   # 回滚后再回滚 = 回到刚放弃的版本
    svc_cmd stop "$SERVICE" 2>/dev/null || true
    flip_restart_commit "$target" "$cur" "$htimeout"
}

cmd_adopt() {  # $1=staged-dir $2=version 其余为选项
    # ci.sh 集成：接管已 rsync 到板端的产物目录，完成布局迁移 + 切换 + 健康。
    require_root
    local staged="$1" ver="$2"; shift 2
    local health=1 htimeout=120
    while [ $# -gt 0 ]; do
        case "$1" in
            --commit|--no-health) health=0 ;;
            --health-timeout) htimeout="$2"; shift ;;
            *) fail "adopt 未知参数：$1" ;;
        esac
        shift
    done
    [ -d "$staged" ] || fail "staged 目录不存在：$staged"
    valid_version "$ver" || fail "版本号非法：$ver（需 X.Y.Z）"
    mkdir -p "$RELEASES"

    # 自刷新：ci.sh scp 覆盖的是 $ROOT 下的本脚本，随后它自己执行 adopt。
    # 这里先把新版落到 $0 旁边——$0 即 $ROOT/weaknet-local-update.sh（install 后
    # 旧 inode 上运行，mv 替换路径）。否则 $0 内容停留在 scp 之前的旧版，
    # apply/adopt 内 install_self_script "$0" 会把旧版拷进 release 再盖回 $0。
    if [ -f "$ROOT/weaknet-local-update.sh" ] && [ "$ROOT/weaknet-local-update.sh" != "$0" ] \
       && [ -f "$0" ] && ! cmp -s "$0" "$ROOT/weaknet-local-update.sh"; then
        install -m 0755 "$0" "$ROOT/weaknet-local-update.sh.new"
        mv -f "$ROOT/weaknet-local-update.sh.new" "$ROOT/weaknet-local-update.sh"
    fi

    # 首次迁移：旧扁平布局整体归档为 releases/legacy-flat（含 VERSION 占位）
    local cur; cur="$(current_version)"
    if [ -z "$cur" ] && [ -d "$ROOT/server" ] && [ ! -L "$ROOT/server" ]; then
        info "检测到旧扁平布局，归档为 releases/legacy-flat"
        mkdir -p "$RELEASES/legacy-flat"
        local e
        for e in server client lib edge_network_rag.py network_knowledge_base.py; do
            [ -e "$ROOT/$e" ] && [ ! -L "$ROOT/$e" ] && mv "$ROOT/$e" "$RELEASES/legacy-flat/" || true
        done
        echo "0.0.0" > "$RELEASES/legacy-flat/VERSION" 2>/dev/null || true
        touch "$RELEASES/legacy-flat/.committed"
    fi

    # staged → releases/<ver>（补 VERSION/manifest/脚本，供后续 rollback 与校验）；
    # 同版本已存在时旧副本保留到健康窗口通过才清理
    local rel="$RELEASES/$ver" old_dir=""
    if [ -d "$rel" ]; then
        old_dir="$RELEASES/.$ver.old.$$"
        rm -rf "$old_dir"
        mv "$rel" "$old_dir"
    fi
    mv "$staged" "$rel"
    echo "$ver" > "$rel/VERSION"
    [ -f "$rel/weaknet-local-update.sh" ] || install -m 0755 "$0" "$rel/weaknet-local-update.sh"
    (cd "$rel" && find . -type f ! -name manifest.sha256 ! -name .committed -print0 \
        | sed -z 's|^\./||' | sort -z | xargs -0 sha256sum > manifest.sha256)
    verify_release_dir "$rel" >/dev/null

    if [ -n "$cur" ]; then echo "$cur" > "$RELEASES/.previous"; else rm -f "$RELEASES/.previous"; fi
    install_self_script "$rel"

    if [ "$health" = "0" ]; then
        atomic_symlink "releases/$ver" "$CURRENT_LINK"
        refresh_compat_links
        svc_cmd daemon-reload || true
        svc_cmd restart "$SERVICE" || warn "restart 返回非零"
        touch "$rel/.committed"
        [ -n "$old_dir" ] && rm -rf "$old_dir"
        warn "--no-health：跳过健康窗口直接转正 releases/${ver}"
        return 0
    fi
    flip_restart_commit "$ver" "$cur" "$htimeout" "$old_dir"
}

cmd_package() {  # <dist-dir> <version> <out.tar.zst>
    local dist="$1" ver="$2" out="$3"
    [ -d "$dist" ] || fail "dist 目录不存在：$dist"
    valid_version "$ver" || fail "版本号非法：$ver（需 X.Y.Z）"
    echo "$ver" > "$dist/VERSION"
    if [ -f "$0" ]; then install -m 0755 "$0" "$dist/weaknet-local-update.sh"; fi
    (cd "$dist" && find . -type f ! -name manifest.sha256 ! -name .committed -print0 \
        | sed -z 's|^\./||' | sort -z | xargs -0 sha256sum > manifest.sha256)
    mkdir -p "$(dirname "$out")"
    tar --zstd --exclude=./.committed -cf "$out" -C "$dist" .
    if [ -n "${WEAKNET_RELEASE_PRIV:-}" ] && [ -f "${WEAKNET_RELEASE_PRIV}" ]; then
        openssl dgst -sha256 -sign "$WEAKNET_RELEASE_PRIV" \
            -out "$out.sig" "$out"
        pass "已签名：$out.sig"
    else
        warn "未设 WEAKNET_RELEASE_PRIV，包未签名（Phase 1 本地信任可接受）"
    fi
    pass "打包完成：$out ($(du -h "$out" | cut -f1))"
}

# ---- 入口 -------------------------------------------------------------------

usage() { sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'; }

main() {
    local cmd="${1:-}"
    [ -n "$cmd" ] || { usage; exit 1; }
    shift || true
    case "$cmd" in
        status)   cmd_status "$@" ;;
        verify)   cmd_verify "$@" ;;
        apply)    cmd_apply "$@" ;;
        rollback) cmd_rollback "$@" ;;
        adopt)    cmd_adopt "$@" ;;
        package)  cmd_package "$@" ;;
        -h|--help|help) usage ;;
        *) usage; fail "未知命令：$cmd" ;;
    esac
}

main "$@"
