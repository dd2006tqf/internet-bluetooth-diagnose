#!/usr/bin/env bash
# =============================================================================
# snapshot_orphan_sweep.sh —— 把「孤儿清扫命令」包装成「可以进定时器的东西」
# =============================================================================
#
# 与 snapshot_retention.sh 是同一套结构，区别只有一个：**判据不同**。
#
#   回收器：对象**有行**，但行太老了        → 按 retention_days 删
#   清扫器：对象**没有行**，但对象太老了     → 按 min_age_seconds 删
#
# 两者删的集合不相交，所以必须**都**跑。只跑回收器时，孤儿对象永远不会被
# 回收（它们连行都没有，回收器根本看不见）；只跑清扫器时，有行的过期证据
# 永远不会被回收。
#
# 为什么这层包装不能省
# --------------------
# 与回收器完全相同的三件事，一件都不能少：
#
#   1. **注入凭据**。systemd oneshot 不继承交互 shell 的环境，缺变量必须
#      立刻失败，而不是跑到一半才发现连不上库。
#   2. **多租户循环**。命令一次只处理一个 --tenant-id。漏掉一个租户 =
#      「这个租户的孤儿永远不回收」，而漏掉**不会报错**：少跑一个租户看起来
#      和「这个租户没有孤儿」一模一样。
#   3. **吃掉退出码 3**。「还有剩余」对调度器意味着「再来一轮」，systemd 的
#      oneshot 不认，只会把 unit 标红。一个每天都红的定时器会让人开始忽略
#      它，而忽略正是磁盘写满的标准前奏。
#
# 为什么 min_age_seconds 要有下限校验
# -----------------------------------
# 这个参数**不是**调优旋钮。上传是「put → insert」，两者之间对象没有行；窗口
# 设短了，清扫器会把**正在上传的图**当成孤儿删掉，然后上传方插入一行指向
# 不存在的对象——正是整条留证链路一直在防的谎报行。
#
# ``SnapshotOrphanSweeper`` 自己已经硬拒绝 < 1，但这里再加一道：配置写错时
# 应该在**开跑之前**就红，而不是等循环进去、连了库、枚举完才开始拒绝。
#
# 部署
# ----
#   sudo install -m 0755 tools/snapshot_orphan_sweep.sh /usr/local/bin/weaknet-snapshot-sweep
#
# 依赖的环境变量（由 unit 的 Environment= 提供，不从 shell 继承）：
#   IOAP_DATABASE_URL          —— 生产数据库 DSN
#   REHEARSAL_OBJECT_STORE_DIR —— 落盘对象存储根目录（与上传端同一个）
#   SNAPSHOT_SWEEP_TENANTS     —— 空格分隔的租户列表（或用 SNAPSHOT_SWEEP_ALL=1）
#   SNAPSHOT_SWEEP_MIN_AGE_SECONDS —— 时间窗；**故意不给默认值**
#
# 为什么窗口时长不给默认值
# ------------------------
# 与保留期天数同一个理由：默认值意味着某次部署忘了配，清扫器会带着一个
# 没人复核过的窗口开始删对象，而且看起来一切正常。宁可 unit 红着。

set -euo pipefail

PROG="industrial-ops-snapshot-sweep"
EXIT_REFUSED=1
EXIT_USAGE=2
EXIT_MORE_WORK=3

# 单轮最多循环多少次。孤儿是崩溃残留（不是流量），一次攒不出几万个；
# 真遇到积压，下一天会接着删。
MAX_ROUNDS=${SNAPSHOT_SWEEP_MAX_ROUNDS:-20}

log() { printf '[snapshot-sweep] %s\n' "$*" >&2; }
die() { log "ERROR: $*"; exit 1; }

# ---- 前置检查：缺任何一个都立刻失败，不等到跑到一半 ----

: "${IOAP_DATABASE_URL:?IOAP_DATABASE_URL is required}"
: "${REHEARSAL_OBJECT_STORE_DIR:?REHEARSAL_OBJECT_STORE_DIR is required}"
: "${SNAPSHOT_SWEEP_MIN_AGE_SECONDS:?SNAPSHOT_SWEEP_MIN_AGE_SECONDS is required (no default: a short window deletes images that are still uploading)}"

[[ "$SNAPSHOT_SWEEP_MIN_AGE_SECONDS" =~ ^[0-9]+$ ]] \
    || die "SNAPSHOT_SWEEP_MIN_AGE_SECONDS must be an integer, got: $SNAPSHOT_SWEEP_MIN_AGE_SECONDS"
(( SNAPSHOT_SWEEP_MIN_AGE_SECONDS >= 60 )) \
    || die "SNAPSHOT_SWEEP_MIN_AGE_SECONDS must be >= 60; anything shorter risks deleting an image that is mid-upload (put done, row not yet committed), which makes the uploader insert a row pointing at nothing"

[[ -d "$REHEARSAL_OBJECT_STORE_DIR" ]] \
    || die "REHEARSAL_OBJECT_STORE_DIR is not a directory: $REHEARSAL_OBJECT_STORE_DIR"

# ---- 租户列表 ----
#
# 与回收器同一个理由：自动枚举把所有租户的孤儿一起删，「漏配」就变成
# 「默认全删」。显式列表让「删哪些租户」是一个必须写下来的决定。

if [[ -n "${SNAPSHOT_SWEEP_TENANTS:-}" ]]; then
    read -r -a TENANTS <<<"$SNAPSHOT_SWEEP_TENANTS"
elif [[ "${SNAPSHOT_SWEEP_ALL:-0}" == "1" && -n "${SNAPSHOT_SWEEP_TENANT:-}" ]]; then
    TENANTS=("$SNAPSHOT_SWEEP_TENANT")
else
    die "set SNAPSHOT_SWEEP_TENANTS='tid1 tid2' (or SNAPSHOT_SWEEP_ALL=1 with SNAPSHOT_SWEEP_TENANT=tid)"
fi

(( ${#TENANTS[@]} > 0 )) || die "tenant list is empty"

log "start: min_age=${SNAPSHOT_SWEEP_MIN_AGE_SECONDS}s tenants=${TENANTS[*]} store=$REHEARSAL_OBJECT_STORE_DIR"

# ---- 逐租户、逐批清扫 ----

for tenant in "${TENANTS[@]}"; do
    round=0
    while :; do
        round=$((round + 1))
        if (( round > MAX_ROUNDS )); then
            log "WARN: tenant=$tenant hit MAX_ROUNDS=$MAX_ROUNDS; remaining work deferred to next timer run"
            break
        fi

        set +e
        output=$("$PROG" --tenant-id "$tenant" --min-age-seconds "$SNAPSHOT_SWEEP_MIN_AGE_SECONDS" --apply 2>&1)
        rc=$?
        set -e

        case $rc in
            0)
                log "tenant=$tenant round=$round done: $output"
                break
                ;;
            "$EXIT_MORE_WORK")
                # 「还有剩余」不是错误，是「继续」。这一句就是整个包装脚本
                # 存在的核心理由：交给 systemd，unit 会红；在这里循环，它只是
                # 一次正常的下一轮。
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
