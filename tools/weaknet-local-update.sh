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
#   WEAKNET_OTA_RECEIPT       云端下发升级时的回执文件路径（上报器注入）；
#                             本脚本在退出时按最终结果改写其中 result 字段
#                             （0→SUCCEEDED，非 0→FAILED），供上报器回传云端
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
    # 真 root 且身处服务 cgroup 时先迁出（apply/rollback/adopt 都会 stop 服务）
    escape_service_cgroup
}

# ---- 云端下发 OTA 回执（WEAKNET_OTA_RECEIPT）----------------------------------
# 云端经 pending_actions 下发升级时，上报器（weaknet-dbus-server）把 action
# 上下文写进回执文件（result=IN_PROGRESS）并通过环境变量把路径传进来。本脚本
# 是终态的唯一写入者：
#   - 成功转正后由 cmd_apply **显式**写 result=SUCCEEDED（唯一成功出口）
#   - EXIT 陷阱只写 FAILED：任何非正常走到成功出口的退出（fail/健康窗口回滚/
#     SIGTERM 连带杀死）一律按失败上报
# 注意：绝不能用"退出码 0"推断成功——bash 死于 SIGTERM 时 EXIT 陷阱里的 $? 是
# 上一条命令的状态（实测为 0），据此写 SUCCEEDED 会把"服务重启把自己杀了"
# 误报成升级成功（真机踩过：板子还在 1.0.0，云端已记 APPLIED）。
OTA_RECEIPT="${WEAKNET_OTA_RECEIPT:-}"
ota_receipt_result() {  # $1 = SUCCEEDED | FAILED
    if [ -n "$OTA_RECEIPT" ] && [ -f "$OTA_RECEIPT" ]; then
        sed -i "s/\"result\":\"IN_PROGRESS\"/\"result\":\"$1\"/" "$OTA_RECEIPT" 2>/dev/null || true
    fi
}
ota_receipt_progress() {  # 重置 stall 计时（上报器按文件 mtime 判定升级进程死活）
    if [ -n "$OTA_RECEIPT" ] && [ -f "$OTA_RECEIPT" ]; then
        touch "$OTA_RECEIPT" 2>/dev/null || true
    fi
}
ota_receipt_on_exit() {
    # 防砖兜底：若死在"已 stop 服务、还没 restart"的窗口内（翻转中途被杀/出错），
    # 把服务拉回来——否则 systemd 对干净 stop 不做自动重启，板子会一直失联。
    if [ "${OTA_SERVICE_STOPPED:-0}" = "1" ] && [ "$DRYRUN" != "1" ]; then
        if [ "$(systemctl is-active "$SERVICE" 2>/dev/null || true)" != "active" ]; then
            systemctl start "$SERVICE" 2>/dev/null || true
        fi
    fi
    # 走到这里且结果仍是 IN_PROGRESS = 没能抵达显式成功出口 → 按失败上报。
    # 不看退出码：SIGTERM 死亡时 $? 不可靠（见上方注释）。
    if [ -n "$OTA_RECEIPT" ] && [ -f "$OTA_RECEIPT" ] && \
       grep -q '"result":"IN_PROGRESS"' "$OTA_RECEIPT" 2>/dev/null; then
        ota_receipt_result FAILED
    fi
    return 0
}

# ---- 服务 cgroup 迁出 --------------------------------------------------------
# 云端下发时升级脚本由 weaknet-dbus-server fork 而来，天然身处
# weaknet-server.service 的 cgroup（setsid 只能脱离会话，脱离不了 cgroup）。
# 升级第一步就是 systemctl stop weaknet-server——KillMode 默认 control-group，
# systemd 会把整个 cgroup（含本脚本）一起 SIGTERM，脚本死在下载完成之后、
# 翻转之前（真机踩过）。迁到根 cgroup 后 stop 服务不再波及升级进程。
# 本地手动执行（ssh/sudo）不在服务 cgroup 内，本函数为无害空操作。
escape_service_cgroup() {
    case "$(cat /proc/self/cgroup 2>/dev/null || true)" in
        *weaknet-server.service*)
            if echo "$$" > /sys/fs/cgroup/cgroup.procs 2>/dev/null; then
                info "已迁出 weaknet-server 服务 cgroup（升级进程不再受 stop 连带影响）"
            else
                fail "无法迁出 weaknet-server 服务 cgroup（cgroup v2 根不可写）——继续执行会在 systemctl stop 时被连带杀死"
            fi
            ;;
    esac
}

# release 目录属主/权限归一化。$1=release 目录
# 服务内 fork 执行时进程受 unit CapabilityBoundingSet 限制（无 CAP_CHOWN），
# chown 改不了属主；且归档根目录权限随构建机 umask 漂移（踩过 700 的坑）
# ——两层叠加把 radxa 锁在 releases/<ver> 外面（weaknet-cli Permission
# denied）。chown 尽力而为（手动 ssh 全权 root 下成功，服务子进程下静默
# 保留 root 属主但功能无碍）；chmod 必成功（root 改自己拥有的文件不需
# CAP_FOWNER）。X 语义：目录与本就有执行位的文件保留 x，其余不加。
normalize_release_perms() {
    local rel="$1" owner
    owner="$(stat -c '%u:%g' "$ROOT" 2>/dev/null || echo "")"
    if [ -n "$owner" ]; then
        chown -R "$owner" "$rel" 2>/dev/null || true
    fi
    find "$rel" -type d -exec chmod 755 {} + 2>/dev/null || true
    find "$rel" -type f -exec chmod u=rwX,go=rX {} + 2>/dev/null || true
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

# 资源回收与历史版本 GC：
# 1) 清扫历史残留的 .staging.* 目录
# 2) 保留当前版本、.previous 版本以及最新的 MAX_RETAIN 个历史版本，淘汰过旧版本
cleanup_staging_and_gc() {
    local cur_target="${1:-}"
    local max_keep="${WEAKNET_RELEASES_MAX_KEEP:-3}"

    # 1. 清扫 .staging 临时残留
    rm -rf "$RELEASES"/.staging.* 2>/dev/null || true

    # 2. GC 历史版本：保留当前激活、previous 以及 legacy-flat
    local cur; cur="$(current_version)"
    local prev; prev="$(cat "$RELEASES/.previous" 2>/dev/null || true)"

    local versions=()
    for d in "$RELEASES"/*/; do
        [ -d "$d" ] || continue
        local v; v="$(basename "$d")"
        if valid_version "$v"; then
            versions+=("$v")
        fi
    done

    # 按 dpkg 版本排序（由新到旧）
    if [ "${#versions[@]}" -gt "$max_keep" ]; then
        local sorted_vers=()
        while IFS= read -r line; do
            [ -n "$line" ] && sorted_vers+=("$line")
        done < <(printf '%s\n' "${versions[@]}" | sort -V -r)

        local keep_count=0
        for v in "${sorted_vers[@]}"; do
            # 保护白名单：当前版本、前一版本、本次刚升级的目标
            if [ "$v" = "$cur" ] || [ "$v" = "$prev" ] || [ "$v" = "$cur_target" ]; then
                continue
            fi
            keep_count=$((keep_count + 1))
            if [ "$keep_count" -ge "$max_keep" ]; then
                info "OTA GC 自动回收历史版本: releases/$v"
                rm -rf "$RELEASES/$v" 2>/dev/null || true
            fi
        done
    fi
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

svc_cmd() {  # systemctl 包装（DRYRUN 下视为成功）；顺带维护"服务被我停了"标记
    if [ "$DRYRUN" = "1" ]; then return 0; fi
    local rc=0
    systemctl "$@" || rc=$?
    if [ "$rc" = "0" ]; then
        case "$1" in
            stop)         OTA_SERVICE_STOPPED=1 ;;
            restart|start) OTA_SERVICE_STOPPED=0 ;;
        esac
    fi
    return "$rc"
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
        cleanup_staging_and_gc "$target"
        pass "升级完成并转正：releases/${target}"
        ota_receipt_result SUCCEEDED  # 唯一成功出口：真转正了才写终态
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
    rm -rf "$staging"; mkdir -m 0755 -p "$staging"
    case "$pkg" in
        # --no-same-owner：解包时按当前用户落属主，不还原归档里的 uid/gid。
        # 云端下发时升级脚本跑在服务子进程里（无 CAP_CHOWN），保留归档属主会
        # 每条文件都报 "Cannot change ownership" 并让 tar 以非零码退出（实测
        # 刷 108 行告警）。属主统一由 normalize_release_perms 归一化。
        *.tar.zst|*.tzst) tar --no-same-owner --zstd -xf "$pkg" -C "$staging" ;;
        *.tar.gz|*.tgz)   tar --no-same-owner -zxf "$pkg" -C "$staging" ;;
        *.tar)            tar --no-same-owner -xf "$pkg" -C "$staging" ;;
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

cmd_verify() {  # $1=pkg|dir|URL 其余为选项
    local target="$1"; shift || true
    local allow_unsigned=0 allow=0
    while [ $# -gt 0 ]; do
        case "$1" in
            --insecure|--allow-unsigned) allow_unsigned=1 ;;
            # 降级策略：apply 用 --allow-downgrade 才放行，verify 同样支持，
            # 否则会出现"verify 说通过、apply 却拒绝降级"的自相矛盾。
            --allow-downgrade) allow=1 ;;
            *) fail "verify 未知参数：$1" ;;
        esac
        shift
    done
    [ -n "$target" ] || fail "用法：verify <pkg.tar.zst|release-dir|http(s)://...> [--insecure] [--allow-downgrade]"
    # 与 apply 同一 URL 拉取路径：URL 入参先下载到临时文件再校验，避免
    # verify 与 apply 行为不一致（verify 只收本地文件、apply 却在线拉包）。
    local downloaded_tmp=""
    if [[ "$target" =~ ^https?:// ]]; then
        info "检测到远程固件 URL: $target，正在下载..."
        downloaded_tmp="$(mktemp /tmp/wn-ota-dl-XXXXXX.tar.zst)"
        curl -fSL --speed-limit 1024 --speed-time 60 "$target" -o "$downloaded_tmp" || {
            rm -f "$downloaded_tmp"
            fail "远程固件包下载失败: $target"
        }
        curl -fSL --speed-limit 1024 --speed-time 60 \
            "${target}.sig" -o "${downloaded_tmp}.sig" 2>/dev/null || true
        target="$downloaded_tmp"
        info "远程固件下载完成，进入本地校验"
    fi
    if [ -d "$target" ]; then
        verify_release_dir "$target" >/dev/null
        [ -n "$downloaded_tmp" ] && rm -f "$downloaded_tmp" "${downloaded_tmp}.sig"
        pass "release 目录校验通过：$target"
    else
        local cur; cur="$(current_version)"
        local staging; staging="$(stage_package "$target" "$allow" "$allow_unsigned")" || {
            [ -n "$downloaded_tmp" ] && rm -f "$downloaded_tmp" "${downloaded_tmp}.sig"
            exit 1
        }
        local ver; ver="$(tr -d '[:space:]' < "$staging/VERSION")"
        rm -rf "$staging"
        [ -n "$downloaded_tmp" ] && rm -f "$downloaded_tmp" "${downloaded_tmp}.sig"
        pass "升级包校验通过：version=${ver}（current=${cur:-无}，策略允许）"
    fi
}

cmd_apply() {  # $1=pkg 其余为选项
    # 回执终态陷阱只在 apply 生命周期内生效（status/rollback 等不写回执）；
    # 放在 require_root 之前：权限失败也要立刻落 FAILED，不等 600s 超时。
    if [ -n "$OTA_RECEIPT" ]; then
        trap ota_receipt_on_exit EXIT
    fi
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
    [ -n "$pkg" ] || fail "用法：apply <pkg.tar.zst|http(s)://...> [--allow-downgrade] [--commit] [--health-timeout N] [--insecure]"

    # 云端下发场景：从现在起维持回执文件的 stall 计时心跳
    ota_receipt_progress

    # 支持直接从 URL 下载远程固件包
    local downloaded_tmp=""
    if [[ "$pkg" =~ ^https?:// ]]; then
        info "检测到远程固件 URL: $pkg，正在下载..."
        downloaded_tmp="$(mktemp /tmp/wn-ota-dl-XXXXXX.tar.zst)"
        # --speed-limit/--speed-time：真停顿 60s 即中止（弱网断流不无限挂）；
        # 后台+轮询心跳：活跃但缓慢的下载持续刷新回执 mtime，避免被上报器
        # 误判为"升级进程已死"。心跳只跟随 curl 进程存活，脚本自身死亡时
        # 循环一并退出，不会留下孤儿心跳掩盖崩溃。
        curl -fSL --speed-limit 1024 --speed-time 60 "$pkg" -o "$downloaded_tmp" &
        local curl_pid=$! curl_rc=0
        while kill -0 "$curl_pid" 2>/dev/null; do
            ota_receipt_progress
            sleep 5   # 短间隔心跳：curl 退出后 5s 内离开循环
        done
        wait "$curl_pid" || curl_rc=$?
        if [ "$curl_rc" -ne 0 ]; then
            rm -f "$downloaded_tmp"
            fail "远程固件包下载失败(rc=$curl_rc): $pkg"
        fi
        # 同时尝试下载分离式签名文件 .sig（小文件，直接同步拉取）
        curl -fSL --speed-limit 1024 --speed-time 60 \
            "${pkg}.sig" -o "${downloaded_tmp}.sig" 2>/dev/null || true
        pkg="$downloaded_tmp"
        info "远程固件下载完成，进入本地验签与应用流程"
        ota_receipt_progress
    fi

    local cur; cur="$(current_version)"
    info "current=${cur:-无（首次/未迁移）} → 应用 $pkg"
    local staging
    if ! staging="$(stage_package "$pkg" "$allow" "$allow_unsigned")"; then
        [ -n "$downloaded_tmp" ] && rm -f "$downloaded_tmp" "${downloaded_tmp}.sig"
        exit 1
    fi
    local ver; ver="$(tr -d '[:space:]' < "$staging/VERSION")"
    [ -n "$downloaded_tmp" ] && rm -f "$downloaded_tmp" "${downloaded_tmp}.sig"
    ota_receipt_progress

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
    normalize_release_perms "$RELEASES/$ver"
    install_self_script "$RELEASES/$ver"

    if [ "$health" = "0" ]; then
        atomic_symlink "releases/$ver" "$CURRENT_LINK"
        refresh_compat_links
        svc_cmd daemon-reload || true
        svc_cmd restart "$SERVICE" || warn "restart 返回非零（--commit 模式不看健康窗口）"
        touch "$RELEASES/$ver/.committed"
        [ -n "$old_dir" ] && rm -rf "$old_dir"
        cleanup_staging_and_gc "$ver"
        warn "--commit：跳过健康窗口，已直接转正 releases/${ver}"
        ota_receipt_result SUCCEEDED  # 唯一成功出口：真转正了才写终态
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
    normalize_release_perms "$rel"
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
    # --owner/--group 归一为 0:0：打包机上的构建 uid（如 1000:1001）不该被
    # 写进制品——板端解包要靠 --no-same-owner + normalize_release_perms 落
    # 属主，归一化后 tar 元数据在任何机器上都可预期。
    tar --zstd --owner=0 --group=0 --exclude=./.committed -cf "$out" -C "$dist" .
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
