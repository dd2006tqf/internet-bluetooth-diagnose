#!/usr/bin/env bash
# =============================================================================
# snapshot_retention.sh —— 把「保留策略命令」包装成「可以进定时器的东西」
# =============================================================================
#
# 为什么需要这层包装，而不是让 unit 直接 ExecStart 那条命令
# ----------------------------------------------------------
# 三件事命令行入口本身不做，而不做就会让定时器骗人：
#
#   1. **注入凭据**。`industrial-ops-snapshot-retention` 从 `IOAP_DATABASE_URL`
#      与 `REHEARSAL_OBJECT_STORE_DIR` 读配置。systemd 的 oneshot 单元不会
#      继承交互 shell 的环境，必须在包装脚本里显式给出，并且缺一个就立刻
#      失败——而不是让它跑到一半才发现连不上库。
#
#   2. **多租户循环**。命令一次只处理一个 `--tenant-id`。生产环境有多个租户，
#      漏掉任何一个都是「有些客户的证据永远不回收」，而漏掉不会报错：
#      少跑一个租户看起来和「这个租户没有过期数据」一模一样。
#
#   3. **吃掉退出码 3**。「还有剩余」对调度器意味着「再来一轮」，但 systemd
#      的 oneshot 不认这个概念——它只会认为「命令失败了」并把 unit 标红。
#      一个每天都红的定时器会让人开始忽略它，而忽略定时器正是磁盘写满的
#      标准前奏。所以循环必须在这里，unit 只该看到 0 或真正的错误。
#
# 为什么退出码 1/2 要原样透出
# ---------------------------
# 1（参数被拒绝）与 2（用法错误）都是**配置问题**，重试一万次也一样。
# 让它们把 unit 标红，是唯一能让「保留期配错」被人类看见的方式。
# 把它们吞掉，磁盘会在配置错误下继续增长，而 `systemctl status` 一直是绿的。
#
# 部署
# ----
#   sudo install -m 0755 tools/snapshot_retention.sh /usr/local/bin/weaknet-snapshot-retention
#
# 依赖的环境变量（由 unit 的 Environment= 提供，不从 shell 继承）：
#   IOAP_DATABASE_URL          —— 生产数据库 DSN
#   REHEARSAL_OBJECT_STORE_DIR —— 落盘对象存储根目录（与上传端同一个）
#   SNAPSHOT_RETENTION_DAYS    —— 保留天数；**故意不给默认值**
#   SNAPSHOT_RETENTION_TENANTS —— 空格分隔的租户列表（或用 SNAPSHOT_RETENTION_ALL=1）
#
# 为什么 SNAPSHOT_RETENTION_DAYS 不给默认值
# -----------------------------------------
# 这条命令是**不可逆的证据销毁**。一个「没配就用 90 天」的默认值意味着：
# 某次部署忘了配，回收器会带着一个没人复核过的数字开始删证据，而且看起来
# 一切正常。宁可 unit 因为缺变量而红着，也不替运维做这个决定。

set -euo pipefail

PROG="industrial-ops-snapshot-retention"
EXIT_REFUSED=1
EXIT_USAGE=2
EXIT_MORE_WORK=3

# 单轮最多循环多少批。防止「删不掉的行走不完」时定时器永远不结束。
# 200 张/批 × 50 轮 = 一轮最多回收 1 万张，足够追上日常产生速度；
# 真遇到积压，第二天会接着删，不需要一次删完。
MAX_ROUNDS=${SNAPSHOT_RETENTION_MAX_ROUNDS:-50}

log() { printf '[snapshot-retention] %s\n' "$*" >&2; }
die() { log "ERROR: $*"; exit 1; }

# ---- 前置检查：缺任何一个都立刻失败，不等到跑到一半 ----

: "${IOAP_DATABASE_URL:?IOAP_DATABASE_URL is required}"
: "${REHEARSAL_OBJECT_STORE_DIR:?REHEARSAL_OBJECT_STORE_DIR is required}"
: "${SNAPSHOT_RETENTION_DAYS:?SNAPSHOT_RETENTION_DAYS is required (no default: this deletes evidence irreversibly)}"

[[ "$SNAPSHOT_RETENTION_DAYS" =~ ^[0-9]+$ ]] \
    || die "SNAPSHOT_RETENTION_DAYS must be a positive integer, got: $SNAPSHOT_RETENTION_DAYS"
(( SNAPSHOT_RETENTION_DAYS >= 1 )) \
    || die "SNAPSHOT_RETENTION_DAYS must be >= 1 (0 would delete everything immediately)"

[[ -d "$REHEARSAL_OBJECT_STORE_DIR" ]] \
    || die "REHEARSAL_OBJECT_STORE_DIR is not a directory: $REHEARSAL_OBJECT_STORE_DIR"

# ---- 租户列表 ----
#
# 为什么默认要求显式列出，而不是「自动枚举所有租户」
# --------------------------------------------------
# 自动枚举看起来更省事，但它把「漏配」变成了「默认全删」：一次误操作指向
# 开发库，枚举出来的每个租户都会被回收。显式列表让「删哪些租户」是一个
# 必须写下来的决定。
#
# SNAPSHOT_RETENTION_ALL=1 是给**单租户部署**准备的逃生门（本项目的实际
# 形状），它要求先显式给出 TENANT，仍然不是「自动发现」。

if [[ -n "${SNAPSHOT_RETENTION_TENANTS:-}" ]]; then
    read -r -a TENANTS <<<"$SNAPSHOT_RETENTION_TENANTS"
elif [[ "${SNAPSHOT_RETENTION_ALL:-0}" == "1" && -n "${SNAPSHOT_RETENTION_TENANT:-}" ]]; then
    TENANTS=("$SNAPSHOT_RETENTION_TENANT")
else
    die "set SNAPSHOT_RETENTION_TENANTS='tid1 tid2' (or SNAPSHOT_RETENTION_ALL=1 with SNAPSHOT_RETENTION_TENANT=tid)"
fi

(( ${#TENANTS[@]} > 0 )) || die "tenant list is empty"

log "start: days=$SNAPSHOT_RETENTION_DAYS tenants=${TENANTS[*]} store=$REHEARSAL_OBJECT_STORE_DIR"

# ---- 逐租户、逐批回收 ----

for tenant in "${TENANTS[@]}"; do
    round=0
    while :; do
        round=$((round + 1))
        if (( round > MAX_ROUNDS )); then
            log "WARN: tenant=$tenant hit MAX_ROUNDS=$MAX_ROUNDS; remaining work deferred to next timer run"
            break
        fi

        set +e
        output=$("$PROG" --tenant-id "$tenant" --retention-days "$SNAPSHOT_RETENTION_DAYS" --apply 2>&1)
        rc=$?
        set -e

        case $rc in
            0)
                log "tenant=$tenant round=$round done: $output"
                break
                ;;
            "$EXIT_MORE_WORK")
                # 「还有剩余」不是错误，是「继续」。这一句就是整个包装脚本
                # 存在的核心理由：把它交给 systemd，unit 会红；在这里循环，
                # 它只是一次正常的下一轮。
                log "tenant=$tenant round=$round more work remains: $output"
                continue
                ;;
            "$EXIT_REFUSED" | "$EXIT_USAGE")
                # 配置错误，原样透出，让 unit 红着等人看。
                log "tenant=$tenant CONFIG ERROR (rc=$rc): $output"
                exit "$rc"
                ;;
            *)
                log "tenant=$tenant FAILED (rc=$rc): $output"
                exit "$rc"
                ;;
        esac
    done
done

log "all tenants done"
