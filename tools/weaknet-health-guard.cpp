// SPDX-License-Identifier: GPL-2.0
/*
 * weaknet-health-guard.cpp - 系统健康看门狗守护进程
 *
 * 解决的问题：多线程网关可能遥测已经停摆、数据库不再落盘，但心跳线程
 * 仍在跑，看门狗照样被喂活，故障因此被掩盖。本程序把喂狗条件从
 * 「进程还活着」升级为「系统健康」。
 *
 * 判据（全部只读，不做任何写操作）：
 *   1. 落库推进    history.db-wal 的 mtime —— 必须判 WAL 而不是主库。
 *                  主库在 WAL 模式下长期静止（实测 28 分钟不变），
 *                  真正在动的是 -wal 文件。判主库会得到恒定的假告警。
 *   2. 采集推进    D-Bus GetEbpfMonitorHealth 的 last_successful_sample_ns。
 *                  注意返回的监控器里有若干时间戳恒为 0（按需启用、
 *                  当前未运行），因此不能要求全部推进，否则恒假告警。
 *   3. 进程活性    /proc/<pid>/stat 的 utime+stime 增量。
 *                  不看单线程 CPU：本项目全部线程 comm 都被截断成同一个
 *                  名字，无法按线程名识别角色（这是设计上的硬约束）。
 *   4. D 状态      /proc/<pid>/task/<tid>/stat 第 3 字段。
 *
 * 16 秒硬约束：硬件超时固定 16 秒、WDIOC_SETTIMEOUT 返回 EINVAL、
 *   CONFIG_WATCHDOG_OPEN_TIMEOUT=0 无宽限期。因此判据周期 2 秒、
 *   去抖 K=2（最坏 4 秒确认），所有阈值都压在 16 秒预算内。
 *
 * 三条退出路径（必须显式区分，混在一起会出人命）：
 *   判据失败      不写 'V'，直接 close()  ->  16 秒后硬件复位
 *   正常停机      写 'V' 再 close()        ->  停用看门狗，不复位
 *   自己崩溃      没机会写 'V'             ->  16 秒后复位（期望行为）
 *
 * 失败 JSON 必须自己写：sunxi-wdt 不实现 bootstatus
 *   （WDIOC_GETBOOTSTATUS 恒返回 0x0、WDIOF_CARDRESET 位从不置位），
 *   内核侧无法事后判断「上一次复位是不是看门狗造成的」。
 *   边界：/dev/sda3 是 write back，fsync 只保证内核->设备，
 *   复位瞬间仍在缓存里的数据块可能丢，这条 JSON 不保证一定留下来。
 */

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

/* ---- 判据参数：全部受 16 秒硬约束 ---- */
constexpr int kTickSeconds     = 2;   /* 判定周期 */
constexpr int kDebounceK       = 2;   /* 连续失败 K 次才认定；不允许 3 */
constexpr int kWalMaxAgeSec    = 12;  /* 落库周期 5s，留 2.4 倍余量 */
constexpr int kSampleMaxAgeSec = 20;  /* 采集周期 10s，留 2 倍余量 */
constexpr int kCpuWindowSec    = 6;   /* 进程总 CPU 观察窗 */
constexpr int kCpuMinTicks     = 1;   /* 窗口内少于 1 tick 视为停滞 */
constexpr int kDStateMaxSec    = 8;   /* 单线程 D 状态持续上限 */
constexpr int kDStateMaxTasks  = 4096;/* 线程数上限，防御性 */

/*
 * 服务不在 active 状态下允许容忍的时长。
 *
 * 为什么必须有这个容忍，而不是「MainPID 为 0 就立刻断喂」：
 *   weaknet-server.service 是 RestartSec=5，一次崩溃自动重启要
 *   5 秒等待 + 2~3 秒启动。若把「进程暂时不在」当成死亡，那么每次
 *   服务自动重启都会把整机打复位 —— OTA 升级、甚至一次正常的
 *   systemctl restart 都会变成重启板子。
 *
 * 12 秒 = RestartSec(5) + 启动(3) + 4 秒余量。
 *
 * 第一版取 8 秒，恰好等于 5+3，数学上「刚好够」，工程上是错的：
 * 板端冷启动要加载 12 个 eBPF 程序、打开 WAL、建 D-Bus 连接，
 * 实测 3~5 秒；只要比预估慢 1 秒，一次普通的 systemd 自动重启
 * 就会变成整机复位。余量不是浪费，是给「启动比预估慢」留的活路。
 *
 * 12 秒仍在 16 秒预算内：容忍期最后一刻才开始断喂，硬件从最后一次
 * ping 起计时，仍有 4 秒到复位。而断喂之前每一轮都喂过狗，
 * 所以实际窗口是「12 秒容忍 + 16 秒硬件」，不冲突。
 */
constexpr int kInactiveGraceSec = 12;

/*
 * systemd 正在对该服务执行一个「非失败」的作业时，视为受控过渡。
 *
 * 为什么需要它，而不是只靠 ci.sh 的维护标记：
 *   标记由 ci.sh 创建，但 ci.sh 是**外部脚本**，它可以被 Ctrl-C、
 *   可以 ssh 断线、可以被别的部署方式替代。而 systemd 自己知道
 *   「我正在停/启这个服务」——`Job` 属性非空即表示有 pending 作业。
 *   这是无需任何外部协作就能观测到的事实。
 *
 *   结论：这条判据**没有采用**。单看 Job 只覆盖作业正在执行的那几秒，
 *   而 ci.sh 的 stop 与 adopt 的 start 之间隔着数分钟空档，作业状态是空的。
 *   真正的解法是维护标记（kMaintFlag）与 OTA 回执（kOtaReceipt）这两条
 *   「系统声明的受控过渡」信号 —— 它们覆盖完整时段，而不是某一瞬间。
 *   保留这段说明是为了让后来者知道这条看起来诱人的捷径已经验证过不成立。
 */

/*
 * 维护模式的有效期上限（按标记文件的 mtime 计）。
 *
 * 为什么维护标记也要过期：
 *   如果 ci.sh 在「创建标记」之后被 Ctrl-C 或断线杀掉，标记会留在板上。
 *   若标记永久有效，guard 就永久无脑喂狗 —— 看门狗在，保护不在，而且
 *   没人知道。这是比误复位更糟的失败模式（误复位至少是响的）。
 *
 *   30 分钟覆盖一次完整部署 / OTA 的最坏耗时；超时后自动回到正常判定，
 *   并在日志里明确喊出来，让运维意识到标记没清。
 */
constexpr int kMaintMaxAgeSec = 30 * 60;

/*
 * OTA 升级过渡：回执文件处于 IN_PROGRESS 且仍在刷新 mtime 时，
 * 视为「系统声明的受控过渡」，暂停「服务缺席」判定。
 *
 * 为什么不能只看维护标记：云端下发的 OTA 没有人工参与，创建标记这件事
 * 没有执行者。而升级本身必然要 stop 服务（下载、校验、翻转指针、
 * 重启、健康窗口），动辄数分钟，远超 12 秒容忍窗口 —— 不识别这个状态，
 * 每次云端升级都会被 guard 判成服务死亡，然后把板子复位在升级中途，
 * 比升级失败还糟（可能落在指针已翻转、release 未转正的位置）。
 *
 * 用 mtime 而不是只看 IN_PROGRESS：升级进程若死掉，receipt 会永远停在
 * IN_PROGRESS。而 weaknet-local-update.sh 每完成一步就 touch 一次
 * （ota_receipt_progress），因此「mtime 新鲜」等于「升级进程还活着」。
 * 这个判据与云端上报器的 stall 判定同源，不会出现两套口径。
 */
constexpr int kOtaProgressMaxAgeSec = 300;

/*
 * 新进程出现后的判据豁免窗口（秒）。
 *
 * 为什么必须有：服务刚启动时，history.db-wal 的 mtime 还是上一次停机前的
 * 时间，last_successful_sample_ns 也还没刷新。若立刻判定，这些「陈旧」
 * 都是从进程还不存在时继承来的，不是它的问题 —— 但它们看起来和真的
 * 停滞一模一样。实测 OTA 升级后正是这种形态：停机数分钟，新进程一起来
 * WAL 的 age 就是几百秒，两个周期后直接复位，升级成功却变成刷机。
 *
 * 12 秒 = 落库周期(5s) × 2 + 余量，足够让新进程写出一轮真实数据。
 * 豁免期内**照常喂狗**（保护在线），只是不累计失败。
 */
constexpr int kRestartWarmupSec = 12;

/*
 * 崩溃循环防护。
 *
 * 豁免窗口有代价：每次 PID 变化都重新豁免，若服务陷入「起来就死」的
 * 循环，豁免会一直续期，看门狗就永久被喂活 —— 这正是本设计要消灭的
 * 「假活」形态。
 *
 * 因此对窗口内的重启次数设上限。20 分钟 6 次远超正常运维
 * （一次 OTA 至多 2 次：翻转后重启 + 健康窗口失败回滚再重启），
 * 而任何真正的崩溃循环都会在几分钟内轻易越过。
 */
constexpr int kRestartWindowSec = 20 * 60;
constexpr int kRestartMaxInWin  = 6;

constexpr const char* kWatchdogDev = "/dev/weaknet_wdt";
constexpr const char* kFallbackDev = "/dev/watchdog1";
constexpr const char* kWalPath     = "/home/radxa/weaknet/data/history.db-wal";
constexpr const char* kFailureJson =
    "/var/lib/weaknet/health-guard-last-failure.json";
constexpr const char* kFailureDir  = "/var/lib/weaknet";
constexpr const char* kServiceName = "weaknet-server";

/*
 * 维护标记：存在时无论判据如何都继续喂狗。
 *
 * 为什么需要它：运维要停 weaknet-server 做维护（手工跑历史查询工具、
 * 换数据库、排障）时，服务不在就等于判据全失败。没有这个开关，
 * 「停服务排障」会直接变成「板子被复位」，排障的人永远看不到现场。
 *
 * 这是**显式的人工意图**，不是自动降级：文件必须由人创建，
 * guard 每轮都会在日志里声明自己正处于维护模式，不会静默放过故障。
 */
constexpr const char* kMaintFlag  = "/var/lib/weaknet/health-guard-maintenance";

/*
 * OTA 升级过程的回执文件，由服务端在拉起 weaknet-local-update.sh 时写入。
 * 路径与 server/src/edge_telemetry_exporter.cpp 的 receipt_file() 保持一致
 * （WEAKNET_DATA_DIR 为空时的默认值）。
 */
constexpr const char* kOtaReceipt = "/home/radxa/weaknet/data/pending_ota_receipt.json";

/*
 * 已触发标记：本程序判定不健康并停止喂狗后写下，整个 boot 生命周期内有效。
 *
 * 为什么必须有它，而不是靠 systemd 的 Restart 策略：
 *   guard 停止喂狗后会退出。systemd 若把它拉起来，新实例会重新
 *   open("/dev/weaknet_wdt") —— 而 open 会触发 .start（使能 + 喂狗），
 *   **复位就被静默取消了**。故障还在，保护却没了，比不加保护更糟：
 *   系统看起来「有看门狗」，实际上永远等不到复位。
 *
 *   因此判定必须在 open 之前先查这个标记。标记带着 boot_id，
 *   复位后 boot_id 变化，标记自动作废，不会把下一次启动也毒住。
 *
 * 也正因如此，重启 guard 不能取消一次已触发的复位。要撤销必须
 * 由人删除标记 —— 这是刻意的：自动机制无权推翻「系统已判定不健康」。
 */
constexpr const char* kTripFlag   = "/var/lib/weaknet/health-guard-tripped";

/* 与 systemd RestartPreventExitStatus 约定对应，见单元文件的同级说明 */
constexpr int kExitTripped = 99;

/* ---- 单项检查状态 ---- */
struct CheckState {
    const char* name;
    int         consecutive_fail;
    int         total_fail;
    bool        ok;
    char        detail[192];
};

CheckState g_checks[] = {
    {"wal_mtime", 0, 0, true, ""},
    {"sample_ns", 0, 0, true, ""},
    {"cpu_delta", 0, 0, true, ""},
    {"d_state",   0, 0, true, ""},
};
constexpr size_t kCheckCount = sizeof(g_checks) / sizeof(g_checks[0]);

volatile std::sig_atomic_t g_stop_requested = 0;

void HandleSignal(int) { g_stop_requested = 1; }

/* ---- 小工具 ---- */

/** 读取整个小文件到缓冲区（NUL 结尾）。 */
bool ReadFile(const char* path, char* buf, size_t len)
{
    int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    ssize_t n = ::read(fd, buf, len - 1);
    ::close(fd);
    if (n < 0) {
        return false;
    }
    buf[n] = '\0';
    return true;
}

/** 执行命令并整段读取 stdout。调用方负责 free(buf)。 */
char* RunCommand(const char* cmd, size_t cap, int* out_rc)
{
    char* buf = static_cast<char*>(malloc(cap));
    if (!buf) {
        if (out_rc) *out_rc = -ENOMEM;
        return nullptr;
    }
    FILE* fp = popen(cmd, "r");
    if (!fp) {
        free(buf);
        if (out_rc) *out_rc = -errno;
        return nullptr;
    }
    size_t total = fread(buf, 1, cap - 1, fp);
    int rc = pclose(fp);
    buf[total] = '\0';
    if (out_rc) *out_rc = rc;
    return buf;
}

int64_t MonotonicNs()
{
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

/* ---- 判据 1：落库推进 ---- */

bool CheckWalMtime(CheckState& st)
{
    struct stat sb {};
    if (::stat(kWalPath, &sb) != 0) {
        snprintf(st.detail, sizeof(st.detail), "stat failed errno=%d", errno);
        return false;
    }
    time_t now = ::time(nullptr);
    long age = static_cast<long>(now - sb.st_mtime);
    snprintf(st.detail, sizeof(st.detail), "age_s=%ld", age);
    return age <= kWalMaxAgeSec;
}

/* ---- 判据 2：采集推进 ---- */

/*
 * 为什么用 popen 调 busctl 而不是直接链接 libdbus：
 *   本项目不依赖 JSON 库，手工拼 D-Bus 调用 + 解析 reply 的代码量远大于
 *   解析 busctl 的一行输出；且本判据 2 秒才跑一次，25ms 的子进程开销
 *   可以接受（板端实测）。
 *
 * 输出形如：s "{\"monitors\":[{"name":...,"last_successful_sample_ns":123,...}]}"
 * 若干监控器的时间戳恒为 0（按需启用、当前未运行）。
 * 判据：非零项中至少 2/3 在 kSampleMaxAgeSec 内推进过。
 *
 * 时间基准：last_successful_sample_ns 是 CLOCK_MONOTONIC 纳秒，
 *   与 MonotonicNs() 同基准，可直接相减。
 */
bool CheckSampleNs(CheckState& st)
{
    static const char* kCmd =
        "busctl --system call com.example.WeakNet /com/example/WeakNet "
        "com.example.WeakNet GetEbpfMonitorHealth 2>/dev/null";

    int rc = 0;
    char* buf = RunCommand(kCmd, 64 * 1024, &rc);
    if (!buf) {
        snprintf(st.detail, sizeof(st.detail), "popen failed");
        return false;
    }
    if (rc != 0 || buf[0] == '\0') {
        snprintf(st.detail, sizeof(st.detail), "busctl rc=%d", rc);
        free(buf);
        return false;
    }

    /*
     * busctl 的输出是 D-Bus 字符串类型的渲染形式：外层 s "..."，而 JSON
     * 内部的每个双引号都被**转义**成 \"。真实输出长这样：
     *
     *   s "{\"monitors\":[{\"name\":\"DnsMonitor\",
     *      \"last_successful_sample_ns\":1510056662029,...}]}"
     *
     * 因此文本里实际出现的是 \"last_successful_sample_ns\":（带反斜杠）。
     * 第一版找的是不转义的 "last_successful_sample_ns":，strstr 永远
     * 不命中 → nonzero 恒为 0 → 判据恒失败。
     *
     * 依赖说明：这里依赖 busctl 对字符串类型使用带转义的引号渲染，
     * 这是 D-Bus 的标准序列化，不是本项目约定。若将来 busctl 换成
     * 直接打印裸 JSON，本键会再次失配 —— 下面的诊断分支专门把
     * 「键失配」与「真的全为 0」区分开，避免再次误报成业务事实。
     */
    static const char* kKey = "\\\"last_successful_sample_ns\\\":";
    const char* p = buf;
    int64_t now_ns = MonotonicNs();
    int nonzero = 0;
    int fresh = 0;

    while (true) {
        const char* hit = strstr(p, kKey);
        if (!hit) {
            break;
        }
        hit += strlen(kKey);
        char* end = nullptr;
        long long v = strtoll(hit, &end, 10);
        if (end != hit && v > 0) {
            ++nonzero;
            int64_t age_ns = now_ns - static_cast<int64_t>(v);
            if (age_ns >= 0 && age_ns <= static_cast<int64_t>(kSampleMaxAgeSec) *
                                      1000000000LL) {
                ++fresh;
            }
        }
        p = end != hit ? end : hit + 1;
    }

    if (nonzero == 0) {
        /*
         * 「扫到 0 项」有两个完全不同的成因，必须分开报告：
         *   a) 键根本没命中 → 接口/格式变了，是本判据自身坏了；
         *   b) 键命中了但值全是 0 → 真的没有活跃监控器。
         * 第一版把两者合并成 "no active monitor"，于是一个**解析 bug**
         * 伪装成了**业务事实**，在板端看起来像是采集真的没跑。
         * 判据自身的失效必须能被看见，否则它的结论无法被信任。
         */
        bool key_found = (strstr(buf, kKey) != nullptr);
        if (!key_found) {
            snprintf(st.detail, sizeof(st.detail),
                     "key not found in busctl output (format changed?)");
        } else {
            snprintf(st.detail, sizeof(st.detail),
                     "no active monitor (all timestamps 0)");
        }
        free(buf);
        return false;
    }

    /* 至少 2/3 的非零项在窗口内推进过 */
    bool ok = (static_cast<long long>(fresh) * 3 >=
               static_cast<long long>(nonzero) * 2);
    snprintf(st.detail, sizeof(st.detail), "fresh=%d nonzero=%d", fresh, nonzero);
    free(buf);
    return ok;
}

/* ---- 判据 3 / 4：进程活性 ---- */

/** 用 systemd 取 MainPID；返回 0 表示拿不到。 */
pid_t GetMainPid()
{
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "systemctl show -p MainPID --value %s 2>/dev/null", kServiceName);
    int rc = 0;
    char* buf = RunCommand(cmd, 64, &rc);
    if (!buf) {
        return 0;
    }
    pid_t pid = static_cast<pid_t>(strtol(buf, nullptr, 10));
    free(buf);
    if (rc != 0 || pid <= 0) {
        return 0;
    }
    return pid;
}

/** 读 /proc/<pid>/stat 的 utime+stime（字段 14、15，1-based）。 */
bool ReadProcCpuTicks(pid_t pid, unsigned long long* out)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", static_cast<int>(pid));
    char buf[1024];
    if (!ReadFile(path, buf, sizeof(buf))) {
        return false;
    }
    /*
     * 字段 2 是 (comm)，可能含空格与右括号，因此定位最后一个 ')' 之后
     * 再按空格切分，才不会错位。
     */
    char* rp = strrchr(buf, ')');
    if (!rp) {
        return false;
    }
    /*
     * rp 指向 ')'，其后是 " S ppid ..."：
     *   rp+1 = ' '
     *   rp+2 = 字段 3，即**状态字符**（S/D/R/Z/T...）
     *   rp+4 = 字段 4，ppid，才是第一个数字字段
     *
     * 字段 3 是一个字符而不是数字，strtoull 在它上面会立即失败
     * （end == p）。第一版从 rp+2 起解析字段 3，于是本函数对**任何**
     * 进程都返回 false —— CPU 判据恒失败，guard 一旦启用就会在
     * 去抖 K=2 后判定不健康并停止喂狗，16 秒后复位，形成重启循环。
     *
     * 为什么这个错误躲过了 78 项单元测试（2026-11-15 板端 dry-run 才发现）：
     * 测试里的 parse_cpu_ticks 是 Python 复刻，用 split() 后按索引取
     * fields[11]/fields[12]，走的是一条完全不同的路径。它验证的是
     * 「应该怎么解析」，不是「代码实际怎么解析」。夹具也从没经过
     * strtoull 循环。所以测试全绿而实现对每个进程都返回 false。
     * 回归守护已加在 TestProcStatParsing.test_source_starts_at_field_4。
     */
    char* p = rp + 2;
    if (p[0] == '\0' || p[1] != ' ') {
        return false;                    /* stat 被截断或格式异常 */
    }
    p += 2;                              /* 跳过状态字符与其后的空格 → 字段 4 */
    unsigned long long utime = 0, stime = 0;
    /* 字段 4 到 13 共 10 个，跳过；第 14、15 是 utime、stime */
    for (int f = 4; f <= 15; ++f) {
        char* end = nullptr;
        unsigned long long v = strtoull(p, &end, 10);
        if (end == p) {
            return false;
        }
        if (f == 14) utime = v;
        if (f == 15) stime = v;
        p = end;
        while (*p == ' ') ++p;
    }
    *out = utime + stime;
    return true;
}

/* 跨轮次保存 CPU 采样 */
struct CpuSample {
    bool                valid = false;
    pid_t               pid = 0;
    unsigned long long  ticks = 0;
    int64_t             at_ns = 0;
};

bool CheckCpuDelta(CheckState& st, pid_t pid, CpuSample& prev)
{
    unsigned long long ticks = 0;
    if (!ReadProcCpuTicks(pid, &ticks)) {
        snprintf(st.detail, sizeof(st.detail), "read stat failed pid=%d",
                 static_cast<int>(pid));
        return false;
    }
    int64_t now = MonotonicNs();
    int64_t window_ns = static_cast<int64_t>(kCpuWindowSec) * 1000000000LL;

    if (!prev.valid || prev.pid != pid || now - prev.at_ns >= window_ns) {
        /* 第一次采样或窗口已满：仅记录基线，本轮不判失败 */
        if (prev.valid && prev.pid == pid && now - prev.at_ns >= window_ns) {
            unsigned long long delta =
                (ticks >= prev.ticks) ? (ticks - prev.ticks) : 0;
            snprintf(st.detail, sizeof(st.detail), "delta=%llu in %ds",
                     delta, kCpuWindowSec);
            prev.ticks = ticks;
            prev.at_ns = now;
            return delta >= static_cast<unsigned long long>(kCpuMinTicks);
        }
        prev.valid = true;
        prev.pid = pid;
        prev.ticks = ticks;
        prev.at_ns = now;
        snprintf(st.detail, sizeof(st.detail), "baseline established");
        return true;
    }

    snprintf(st.detail, sizeof(st.detail), "warming up");
    return true;
}

/*
 * 统计进程内处于 D 状态的线程数。
 *
 * 为什么不用 awk 取第 3 字段再 grep 计数：
 * 字段 2 是 (comm)，comm 含空格时按空白切分会整体错位。例如
 * "1234 (my service name) D ..." 的第三段是 "service" 而不是 "D"，
 * 于是卡在 D 的线程数不出来，判据恒通过。错的方向是漏报，也就是
 * 「保护看起来在、实际不在」，比误报危险得多。
 * 同一文件里 CPU 判据已经用 strrchr(')') 正确定位（见 ReadProcCpuTicks），
 * 这里必须用同一套解析，不能一个文件两套做法。
 *
 * 为什么不用 shell 管道：popen 的退出码被丢掉后，扫描失败时计数命令
 * 同样输出 0，会被当成「没有 D 线程」而判通过。直接读目录才能在
 * 失败时返回 -1，与调用方的约定一致。
 */
int CountDThreads(pid_t pid)
{
    if (pid <= 0) {
        return -1;
    }
    char taskdir[64];
    snprintf(taskdir, sizeof(taskdir), "/proc/%d/task", static_cast<int>(pid));
    DIR* dir = opendir(taskdir);
    if (!dir) {
        return -1;
    }

    int n = 0;
    int scanned = 0;
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_name[0] < '0' || ent->d_name[0] > '9') {
            continue;                        /* 跳过 . / .. 与非 tid 项 */
        }
        if (++scanned > kDStateMaxTasks) {
            closedir(dir);                   /* 异常线程数不静默当 0 */
            return -1;
        }
        char path[sizeof(taskdir) + NAME_MAX + 8];
        snprintf(path, sizeof(path), "%s/%s/stat", taskdir, ent->d_name);
        char buf[512];
        if (!ReadFile(path, buf, sizeof(buf))) {
            continue;                        /* 线程已退出：正常竞态，跳过 */
        }
        char* rp = strrchr(buf, ')');
        if (!rp || rp[1] != ' ' || rp[2] == '\0') {
            continue;                        /* 格式异常，跳过该线程 */
        }
        if (rp[2] == 'D') {                  /* rp+2 即字段 3（状态字符） */
            ++n;
        }
    }
    closedir(dir);
    return n;
}

/*
 * D 状态判据：必须判「持续」，不能判瞬时。
 *
 * 一次瞬时 IO 也会出现 D；如果见 D 就判失败，正常盘写抖动就会误复位。
 * 真正的判据是「某个线程在 D 停留超过 kDStateMaxSec」。
 *
 * 实现方式：每轮记录「开始处于 D 的时刻」，D 消失即清零。
 * 本轮轮询若发现某个 D 已持续超过阈值，直接判失败（不再等去抖，
 * 因为时间本身已经是最好的去抖）。
 *
 * 注意：这里跟踪的是「进程内是否存在 D 线程」的最早起始时刻，而不是
 * 逐 tid 追踪。这样一个线程的 D 结束、另一个线程的 D 开始，会被算成
 * 一段连续的 D。这是刻意的简化 —— 按 tid 追踪需要维护 TID 集合，
 * 而判据只需要「有线程长期卡在 D」这一个信号。
 */
bool CheckDState(CheckState& st, pid_t pid)
{
    static int64_t d_since_ns = 0;

    int d_count = CountDThreads(pid);
    if (d_count < 0) {
        snprintf(st.detail, sizeof(st.detail), "scan failed");
        d_since_ns = 0;
        return false;
    }

    if (d_count == 0) {
        d_since_ns = 0;
        snprintf(st.detail, sizeof(st.detail), "d_threads=0");
        return true;
    }

    int64_t now = MonotonicNs();
    if (d_since_ns == 0) {
        d_since_ns = now;
        snprintf(st.detail, sizeof(st.detail), "d_threads=%d starting", d_count);
        return true;   /* 刚进入 D，还没到阈值 */
    }

    int64_t dur_s = (now - d_since_ns) / 1000000000LL;
    snprintf(st.detail, sizeof(st.detail), "d_threads=%d for %llds",
             d_count, static_cast<long long>(dur_s));
    return dur_s < static_cast<int64_t>(kDStateMaxSec);
}

/* ---- 失败 JSON ---- */

/* 定义见下方「维护与触发标记」一节；这里前置声明以便复用 */
void ReadBootId(char* buf, size_t len);

/*
 * 写失败现场快照。
 *
 * reason 用于区分责任：service_absent（服务真没了）/
 * criteria_unhealthy（进程活着但判据挂）/ feed_write_failed（连喂都写不进去）。
 * 三者的运维处置完全不同，混成一个字段就等于放弃了事后归因。
 */
void WriteFailureJson(pid_t pid, const char* reason)
{
    ::mkdir(kFailureDir, 0755);
    int fd = ::open(kFailureJson, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return;
    }

    char boot_id[64];
    ReadBootId(boot_id, sizeof(boot_id));

    char uptime[64] = "0";
    ReadFile("/proc/uptime", uptime, sizeof(uptime));
    char* sp = strchr(uptime, ' ');
    if (sp) *sp = '\0';

    char ts[64];
    time_t now = ::time(nullptr);
    struct tm tmv {};
    gmtime_r(&now, &tmv);
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tmv);

    char out[2048];
    int n = snprintf(out, sizeof(out),
        "{\n"
        "  \"detected_at\": \"%s\",\n"
        "  \"uptime_s\": %s,\n"
        "  \"boot_id\": \"%s\",\n"
        "  \"pid\": %d,\n"
        "  \"reason\": \"%s\",\n"
        "  \"checks\": {\n"
        "    \"wal_mtime\":  {\"ok\": %s, \"detail\": \"%s\", \"fails\": %d},\n"
        "    \"sample_ns\":  {\"ok\": %s, \"detail\": \"%s\", \"fails\": %d},\n"
        "    \"cpu_delta\":  {\"ok\": %s, \"detail\": \"%s\", \"fails\": %d},\n"
        "    \"d_state\":    {\"ok\": %s, \"detail\": \"%s\", \"fails\": %d}\n"
        "  },\n"
        "  \"action\": \"stopped_petting\",\n"
        "  \"reset_expected_in_s\": 16\n"
        "}\n",
        ts, uptime, boot_id, static_cast<int>(pid), reason,
        g_checks[0].ok ? "true" : "false", g_checks[0].detail,
        g_checks[0].consecutive_fail,
        g_checks[1].ok ? "true" : "false", g_checks[1].detail,
        g_checks[1].consecutive_fail,
        g_checks[2].ok ? "true" : "false", g_checks[2].detail,
        g_checks[2].consecutive_fail,
        g_checks[3].ok ? "true" : "false", g_checks[3].detail,
        g_checks[3].consecutive_fail);

    if (n > 0) {
        ssize_t w = ::write(fd, out, static_cast<size_t>(n));
        (void)w;
        /* fsync 只保证内核->设备；write back 下仍可能丢，这是已知边界 */
        ::fsync(fd);
    }
    ::close(fd);
}

/* ---- 主逻辑 ---- */

int OpenWatchdog()
{
    /* 首选专用设备；回退到 watchdog1（与 unbind 后的编号约定一致） */
    int fd = ::open(kWatchdogDev, O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
        return fd;
    }
    fd = ::open(kFallbackDev, O_WRONLY | O_CLOEXEC);
    return fd;
}

/*
 * 维护标记是否有效（人工显式创建，见 kMaintFlag / kMaintMaxAgeSec 注释）。
 *
 * 不看 mtime 是绝对错误的：标记一旦因 Ctrl-C 残留，保护就永久消失。
 * 过期时在 out_reason 里说明原因，由调用方打进日志。
 */
bool MaintenanceRequested(char* out_reason, size_t len)
{
    struct stat sb {};
    if (::stat(kMaintFlag, &sb) != 0) {
        snprintf(out_reason, len, "absent");
        return false;
    }
    time_t now = ::time(nullptr);
    long age = static_cast<long>(now - sb.st_mtime);
    if (age < 0) {
        /* 时钟回跳（无 RTC 的板子第一次 NTP 校正会发生）：保守地按新鲜处理 */
        snprintf(out_reason, len, "present (age unknown, clock stepped)");
        return true;
    }
    if (age > kMaintMaxAgeSec) {
        snprintf(out_reason, len, "EXPIRED age=%lds > %ds, ignoring",
                 age, kMaintMaxAgeSec);
        return false;
    }
    snprintf(out_reason, len, "present age=%lds", age);
    return true;
}

/*
 * OTA 是否正在受控进行（见 kOtaProgressMaxAgeSec 注释）。
 * 返回 true 时把进度写进 out_detail 供日志使用。
 */
bool OtaUpgradeInProgress(char* out_detail, size_t len)
{
    struct stat sb {};
    if (::stat(kOtaReceipt, &sb) != 0) {
        snprintf(out_detail, len, "no receipt");
        return false;
    }

    char buf[4096];
    if (!ReadFile(kOtaReceipt, buf, sizeof(buf))) {
        snprintf(out_detail, len, "receipt unreadable");
        return false;
    }
    if (!strstr(buf, "\"IN_PROGRESS\"")) {
        snprintf(out_detail, len, "receipt finished");
        return false;
    }

    time_t now = ::time(nullptr);
    long age = static_cast<long>(now - sb.st_mtime);
    if (age < 0) {
        age = 0;
    }
    if (age > kOtaProgressMaxAgeSec) {
        /*
         * 回执停在 IN_PROGRESS 却不再刷新 = 升级进程已死。
         * 此时**必须**恢复判定：把死掉的升级当成「受控过渡」，
         * 等于让一次崩溃的升级永久关掉保护。
         */
        snprintf(out_detail, len, "STALLED age=%lds > %ds",
                 age, kOtaProgressMaxAgeSec);
        return false;
    }

    snprintf(out_detail, len, "IN_PROGRESS age=%lds", age);
    return true;
}

/** 读当前 boot_id 到 buf（去尾部换行）。读不到时返回 "unknown"。 */
void ReadBootId(char* buf, size_t len)
{
    snprintf(buf, len, "unknown");
    if (!ReadFile("/proc/sys/kernel/random/boot_id", buf, len)) {
        snprintf(buf, len, "unknown");
        return;
    }
    size_t l = strlen(buf);
    while (l > 0 && (buf[l - 1] == '\n' || buf[l - 1] == '\r')) {
        buf[--l] = '\0';
    }
}

/**
 * 本次启动是否已经触发过（判定不健康、已停止喂狗）。
 *
 * 只要标记里的 boot_id 与当前一致，就返回 true —— 包括 open 之前。
 * 这是「重启无法取消复位」的唯一保证，见 kTripFlag 注释。
 */
bool AlreadyTrippedThisBoot()
{
    char cur[64];
    ReadBootId(cur, sizeof(cur));

    char saved[64];
    if (!ReadFile(kTripFlag, saved, sizeof(saved))) {
        return false;
    }
    size_t l = strlen(saved);
    while (l > 0 && (saved[l - 1] == '\n' || saved[l - 1] == '\r')) {
        saved[--l] = '\0';
    }

    /* boot_id 是 UUID，直接逐字比较即可 */
    return strcmp(saved, cur) == 0;
}

/** 记录本次启动已触发。失败不致命：本进程仍会停止喂狗。 */
void RecordTrip()
{
    ::mkdir(kFailureDir, 0755);
    int fd = ::open(kTripFlag, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return;
    }
    char bid[64];
    ReadBootId(bid, sizeof(bid));
    char line[80];
    int n = snprintf(line, sizeof(line), "%s\n", bid);
    if (n > 0) {
        ssize_t w = ::write(fd, line, static_cast<size_t>(n));
        (void)w;
        ::fsync(fd);
    }
    ::close(fd);
}

void LogCheckRound(int round)
{
    fprintf(stderr, "[round %d]", round);
    for (size_t i = 0; i < kCheckCount; ++i) {
        fprintf(stderr, " %s=%s(%s,f%d)", g_checks[i].name,
                g_checks[i].ok ? "ok" : "FAIL",
                g_checks[i].detail, g_checks[i].consecutive_fail);
    }
    fprintf(stderr, "\n");
}

/*
 * 进程重启跟踪（供 kRestartWarmupSec / kRestartMaxInWin 使用）。
 *
 * 记录最近的 PID 变化时刻，用于两件事：
 *   1. 新 PID 出现后的 kRestartWarmupSec 内跳过判据（不累计失败）
 *   2. 统计 kRestartWindowSec 内的重启次数，超过 kRestartMaxInWin 就
 *      取消豁免 —— 崩溃循环不能被当成「刚起来」无限容忍
 */
struct RestartTracker {
    pid_t   last_pid = 0;
    int64_t changed_at_ns = 0;
    int64_t win_start_ns = 0;
    int     count_in_win = 0;
};

/**
 * 观测到某个 PID，更新跟踪器。
 * 返回 true 表示本轮处于豁免窗口内（调用方应跳过判据累计）。
 */
bool InRestartWarmup(RestartTracker& rt, pid_t pid, char* out_detail, size_t len)
{
    int64_t now = MonotonicNs();

    if (rt.last_pid != pid) {
        /* 首次见到或 PID 变了：算一次重启 */
        if (rt.last_pid != 0) {
            ++rt.count_in_win;
        } else {
            /* 本进程启动时的第一次观测：窗口从此刻开始 */
            rt.win_start_ns = now;
            rt.count_in_win = 0;
        }
        /* 窗口滑动：超出窗口就重置计数 */
        if (rt.win_start_ns != 0 &&
            now - rt.win_start_ns > static_cast<int64_t>(kRestartWindowSec) *
                                       1000000000LL) {
            rt.win_start_ns = now;
            rt.count_in_win = 1;
        }
        rt.last_pid = pid;
        rt.changed_at_ns = now;
    }

    bool warm = (now - rt.changed_at_ns) <
                static_cast<int64_t>(kRestartWarmupSec) * 1000000000LL;

    /*
     * 崩溃循环保护：窗口内重启过多，取消豁免。
     * 此时 warm 强制为 false，判据立刻开始累计失败。
     */
    bool looped = rt.count_in_win > kRestartMaxInWin;
    if (looped) {
        warm = false;
    }

    long long age_s = (now - rt.changed_at_ns) / 1000000000LL;
    snprintf(out_detail, len, "pid=%d age=%llds restarts=%d/%ds%s%s",
             static_cast<int>(pid), age_s, rt.count_in_win, kRestartWindowSec,
             warm ? " WARMUP" : "", looped ? " CRASH_LOOP" : "");
    return warm;
}

}  // namespace

int main(int argc, char** argv)
{
    bool dry_run = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--dry-run") == 0) {
            dry_run = true;
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("用法: %s [--dry-run]\n"
                   "  --dry-run  只打印判据结果，不打开看门狗设备\n",
                   argv[0]);
            return 0;
        }
    }

    struct sigaction sa {};
    sa.sa_handler = HandleSignal;
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);

    /*
     * 必须在 open() 之前检查：open(/dev/watchdog*) 会触发 .start，
     * 也就是「使能 + 喂狗」。一旦被 systemd 拉起来就重新开设备，
     * 上一次判定触发的复位会被静默取消。见 kTripFlag 注释。
     */
    if (!dry_run && AlreadyTrippedThisBoot()) {
        fprintf(stderr,
                "本次启动已触发过健康看门狗（%s 记录了相同 boot_id），"
                "拒绝重新打开设备，让硬件复位按计划发生。\n"
                "如需解除：先排查故障，再删除 %s 并重启本服务。\n",
                kTripFlag, kTripFlag);
        return kExitTripped;
    }

    int wd_fd = -1;
    if (!dry_run) {
        wd_fd = OpenWatchdog();
        if (wd_fd < 0) {
            fprintf(stderr,
                    "无法打开看门狗设备（%s 或 %s）：%s\n",
                    kWatchdogDev, kFallbackDev, strerror(errno));
            return 1;
        }
        /* 启动时立刻喂第一次，缩短「无喂狗」窗口 */
        if (::write(wd_fd, "1", 1) != 1) {
            fprintf(stderr, "首次喂狗失败：%s\n", strerror(errno));
            ::close(wd_fd);
            return 1;
        }
    }

    CpuSample cpu_prev;
    int round = 0;
    int64_t inactive_since_ns = 0;   /* 服务首次被发现不在 active 的时刻 */
    RestartTracker restart_tracker;  /* 见 InRestartWarmup */

    while (!g_stop_requested) {
        ++round;

        /* 维护模式优先于一切判据：标记有效就只喂狗不做判定 */
        char maint_detail[128];
        if (MaintenanceRequested(maint_detail, sizeof(maint_detail))) {
            if (inactive_since_ns != 0) {
                inactive_since_ns = 0;
            }
            fprintf(stderr,
                    "[round %d] MAINTENANCE (%s) -> feeding, judgement disabled\n",
                    round, maint_detail);
            if (!dry_run && ::write(wd_fd, "1", 1) != 1) {
                fprintf(stderr, "[round %d] feed failed: %s\n",
                        round, strerror(errno));
                WriteFailureJson(0, "feed_write_failed");
                ::close(wd_fd);
                return 1;
            }
            for (int s = 0; s < kTickSeconds && !g_stop_requested; ++s) {
                sleep(1);
            }
            continue;
        }

        pid_t pid = GetMainPid();
        if (pid <= 0) {
            /*
             * 先看是不是受控过渡（OTA 正在推进）。
             * 升级期间服务必然缺席，且时长可达数分钟，任何固定宽限都不够。
             * 判据交给升级自己的进度刷新，见 kOtaProgressMaxAgeSec。
             */
            char ota_detail[128];
            if (OtaUpgradeInProgress(ota_detail, sizeof(ota_detail))) {
                inactive_since_ns = 0;
                fprintf(stderr,
                        "[round %d] service %s absent but OTA in progress (%s) "
                        "-> keep feeding\n",
                        round, kServiceName, ota_detail);
                if (!dry_run && ::write(wd_fd, "1", 1) != 1) {
                    fprintf(stderr, "[round %d] feed failed: %s\n",
                            round, strerror(errno));
                    WriteFailureJson(0, "feed_write_failed");
                    ::close(wd_fd);
                    return 1;
                }
                for (int s = 0; s < kTickSeconds && !g_stop_requested; ++s) {
                    sleep(1);
                }
                continue;
            }

            /*
             * 服务不在 active：不立即断喂，先给自动重启留出容忍窗口。
             * 理由见 kInactiveGraceSec 注释 —— 立刻断喂会让每一次
             * systemctl restart / 自动重启都变成整机复位。
             */
            int64_t now = MonotonicNs();
            if (inactive_since_ns == 0) {
                inactive_since_ns = now;
            }
            int64_t gone_s = (now - inactive_since_ns) / 1000000000LL;

            if (gone_s < kInactiveGraceSec) {
                fprintf(stderr,
                        "[round %d] service %s not active for %llds "
                        "(grace %ds) -> keep feeding\n",
                        round, kServiceName, static_cast<long long>(gone_s),
                        kInactiveGraceSec);
                if (!dry_run && ::write(wd_fd, "1", 1) != 1) {
                    fprintf(stderr, "[round %d] feed failed: %s\n",
                            round, strerror(errno));
                    WriteFailureJson(0, "feed_write_failed");
                    ::close(wd_fd);
                    return 1;
                }
            } else {
                fprintf(stderr,
                        "[round %d] service %s absent for %llds (> grace %ds) "
                        "-> stop feeding, expect reset in ~16 s\n",
                        round, kServiceName, static_cast<long long>(gone_s),
                        kInactiveGraceSec);
                if (!dry_run) {
                    RecordTrip();
                    WriteFailureJson(0, "service_absent");
                    ::close(wd_fd);   /* 不写 'V'，16 秒后复位 */
                    return kExitTripped;
                }
            }
        } else {
            inactive_since_ns = 0;

            /*
             * 新 PID 的豁免窗口：见 kRestartWarmupSec 注释。
             * 豁免期内照常喂狗、不累计失败 —— 但也不能无视崩溃循环，
             * 那由 InRestartWarmup 内部的计数上限兜住。
             */
            char warm_detail[160];
            bool warming = InRestartWarmup(restart_tracker, pid,
                                           warm_detail, sizeof(warm_detail));
            if (warming) {
                fprintf(stderr,
                        "[round %d] WARMUP %s -> feeding, judgement suspended\n",
                        round, warm_detail);
                if (!dry_run && ::write(wd_fd, "1", 1) != 1) {
                    fprintf(stderr, "[round %d] feed failed: %s\n",
                            round, strerror(errno));
                    WriteFailureJson(pid, "feed_write_failed");
                    ::close(wd_fd);
                    return 1;
                }
                for (int s = 0; s < kTickSeconds && !g_stop_requested; ++s) {
                    sleep(1);
                }
                continue;
            }

            g_checks[0].ok = CheckWalMtime(g_checks[0]);
            g_checks[1].ok = CheckSampleNs(g_checks[1]);
            g_checks[2].ok = CheckCpuDelta(g_checks[2], pid, cpu_prev);
            g_checks[3].ok = CheckDState(g_checks[3], pid);

            bool any_persistent_fail = false;
            for (size_t i = 0; i < kCheckCount; ++i) {
                if (g_checks[i].ok) {
                    g_checks[i].consecutive_fail = 0;
                } else {
                    ++g_checks[i].consecutive_fail;
                    ++g_checks[i].total_fail;
                    if (g_checks[i].consecutive_fail >= kDebounceK) {
                        any_persistent_fail = true;
                    }
                }
            }

            LogCheckRound(round);

            if (any_persistent_fail) {
                fprintf(stderr,
                        "[round %d] persistent failure -> stop feeding, "
                        "expect reset in ~%d s\n", round, 16);
                if (!dry_run) {
                    RecordTrip();
                    WriteFailureJson(pid, "criteria_unhealthy");
                    ::close(wd_fd);   /* 不写 'V' */
                    return kExitTripped;
                }
            } else if (!dry_run) {
                if (::write(wd_fd, "1", 1) != 1) {
                    fprintf(stderr, "[round %d] feed failed: %s\n",
                            round, strerror(errno));
                    WriteFailureJson(pid, "feed_write_failed");
                    ::close(wd_fd);
                    return 1;
                }
            }
        }

        for (int s = 0; s < kTickSeconds && !g_stop_requested; ++s) {
            sleep(1);
        }
    }

    /* 正常停机：magic close，停用看门狗，不复位 */
    if (!dry_run && wd_fd >= 0) {
        if (::write(wd_fd, "V", 1) != 1) {
            fprintf(stderr, "magic close write failed: %s\n", strerror(errno));
        }
        ::close(wd_fd);
    }
    fprintf(stderr, "graceful shutdown done\n");
    return 0;
}
