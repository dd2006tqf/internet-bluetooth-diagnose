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
 * 路线：不改动内核，直接使用标准看门狗设备 /dev/watchdog0。
 *   平台驱动 sunxi_wdt 全程在线，负责使能、配置、喂狗这几条寄存器写；
 *   本程序不碰任何寄存器，只通过字符设备的标准 write/ioctl 语义驱动它。
 *   板端实测 /dev/watchdog0 是 (10,130) 的 misc 节点，同时存在 (246,0)
 *   的动态节点，两者指向同一个 watchdog_device。
 *
 * 喂狗权是怎么转移的（内核源码级事实，drivers/watchdog/watchdog_dev.c）：
 *   * open() 调 watchdog_start()，置 WDOG_ACTIVE 并使能硬件；
 *   * watchdog_need_worker() 只在「用户态未激活」时代喂：
 *       (hm && active && t > hm) || (t && !active && hw_running)
 *     sunxi_wdt 不设 max_hw_heartbeat_ms（hm = 0），第一项恒假；
 *     open 之后 WDOG_ACTIVE 已置位，第二项也恒假 ⇒ 内核彻底停止代喂。
 *   这就是「不写就没人写」的机制来源，不是约定俗成。
 *
 * 四条退出路径（必须显式区分，混在一起会出人命）：
 *   判据失败      不写 'V'，直接 close()  ->  内核拒绝停狗、卸掉代喂 -> 硬件超时复位
 *   正常停机      写 'V' 再 close()        ->  内核调 ops->stop() 停狗 -> 不复位
 *   自己崩溃      没机会写 'V'（内核代关 fd，走同一条 release）
 *                                          ->  硬件超时复位（期望行为）
 *   连续复位熔断  **不 open 设备**直接退出（退出码 98）
 *                                          ->  设备保持在线，无硬件保护
 *   前两条依赖驱动的 WDIOF_MAGICCLOSE，见下面的契约断言。
 *   第四条是「保护本身在制造故障」时的主动退让，见 kTripHistory 一节。
 *
 * MAGICCLOSE 是本路线唯一的外部依赖，必须在启动时硬断言：
 *   若驱动不支持，watchdog_release() 会无条件 watchdog_stop()，
 *   于是「不写 'V' 直接 close」也会把狗停掉 —— 判据失败不再复位，
 *   保护看起来在、实际不在。VerifyDeviceContract() 不满足就拒绝启动。
 *
 * 超时预算：硬件超时由驱动决定（本板实测 16 秒），判据周期 2 秒、
 *   去抖 K=2（最坏 4 秒确认）。启动时读 WDIOC_GETTIMEOUT 做下限断言，
 *   低于 2×去抖窗口就拒绝启动，避免确认还没跑完板子先复位。
 *   实测从最后一次喂狗到整机复位的端到端延迟约 28 秒（含调度与重启），
 *   因此本文件与文档一律写「由硬件超时复位」，不承诺具体秒数。
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
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <linux/watchdog.h>

namespace {

/* ---- 判据参数：全部受 16 秒硬约束 ---- */
constexpr int kTickSeconds     = 2;   /* 判定周期 */
constexpr int kDebounceK       = 2;   /* 连续失败 K 次才认定；不允许 3 */

/*
 * 单条外部命令的硬上限（毫秒）。
 *
 * 为什么必须有：RunCommand 走 popen/pclose，子进程不退出就是永久阻塞。
 * 判据 2 与判据 3 各要起一个子进程（busctl / systemctl），任一个挂死都会
 * 让主循环停在 pclose 上 —— 不再喂狗，16 秒后整机复位，而且
 * WriteFailureJson 根本执行不到，运维只看到「板子无缘无故重启了」。
 * 这等于守卫把自己的故障伪装成被监控对象的故障，是设计上不可接受的。
 *
 * 为什么是 1500 而不是更小的 500：板端 busctl 实测 25ms，但 SQLite
 * checkpoint 叠加 12 个 eBPF 程序时 D-Bus 应答可能抖到几百毫秒。
 * 1500ms 是实测值的 60 倍，足够吸收抖动。
 *
 * 预算推导（受 16 秒硬约束）：
 *   kCheckCount × kCmdTimeoutMs + kTickSeconds×1000 < 16000
 *        4 × 1500      +        2000         = 8000 ms
 * 余量 50%。实际只有 2 个判据起子进程，真实最坏 2×1500+2000 = 5000 ms。
 * 改动这个值时必须同时复核 TestCommandTimeout 里的预算断言。
 */
constexpr int kCmdTimeoutMs    = 1500;
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
 * 「无法观测到 MainPID」的容忍轮数。
 *
 * 为什么需要一个独立的计数器，而不是让 systemctl 超时直接走
 * 「服务缺席」那条 12 秒宽限：
 *   两者含义完全不同。服务缺席 = 我们知道进程没了；systemctl 超时 =
 *   我们不知道进程还在不在（systemd 自己卡了）。后者若走缺席路径，
 *   守卫会用「复位整机」来惩罚一次 systemd 的抖动，而 systemd 重启后
 *   守卫又会被拉起 —— 典型的自己打自己。
 *
 * 但也不能无限容忍：systemctl 永久挂死时，若一直当「无法观测」继续
 * 喂狗，保护就永久失效了。所以给一个有界容忍：3 轮 = 6 秒（每轮
 * kTickSeconds），超了就按判据失败处理，reason 写 pid_unobservable。
 *
 * 6 秒的选择：必须显著小于 kInactiveGraceSec(12)，否则「先容忍再判定」
 * 的总耗时会挤占 16 秒硬件预算；也不能小于 2（一次去抖的长度），否则
 * 一次 EDQUOT/ENOMEM 抖动就会直接停喂。
 */
constexpr int kPidUnknownMaxStreak = 3;

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

/*
 * 看门狗设备：标准 watchdog 字符设备，由平台驱动 sunxi_wdt 提供。
 *
 * 首选 misc 节点 /dev/watchdog（10,130）。板端同时存在动态编号的
 * /dev/watchdog0（246,0），两者由同一个 watchdog_device 导出，语义等价；
 * 首选 misc 是为了在 devtmpfs 尚未就绪的极早期也能打开。
 *
 * 为什么不做「扫描 /dev/watchdog* 取最小号」：本程序必须精确知道自己
 * 打开的是哪个设备。设备被别的进程占用时（内核强制 single-open，
 * 第二次 open 返回 -EBUSY），盲目试下一个号会让「设备被占」这个
 * 重要信号被悄悄吞掉。
 */
constexpr const char* kWatchdogDev = "/dev/watchdog";
constexpr const char* kWatchdogAlt = "/dev/watchdog0";
constexpr const char* kWalPath     = "/home/radxa/weaknet/data/history.db-wal";

/*
 * 数据目录：WAL、pending_ota_receipt.json 都在这里，由 weaknet-server 写。
 * 与 kFailureDir 分开采样磁盘水位 —— 二者常常不在同一个文件系统上
 * （见 DiskWatermark 一节的第 2 条理由）。
 */
constexpr const char* kDataDir     = "/home/radxa/weaknet/data";

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
 *   open("/dev/watchdog") —— 而 open 会触发 watchdog_start（使能 + 喂狗），
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

/*
 * 连续复位熔断：跨 boot 的触发历史。
 *
 * 为什么必须有：kTripFlag 用 boot_id 做失效判据，这在「重启撤销复位」这条
 *   上是正确的，但它同时意味着**没有任何跨 boot 的记忆**。于是确定性故障
 *   （配置写坏、数据库损坏、一次失败的 OTA）会变成：
 *
 *     boot 1 判据失败 → 停喂 → 复位
 *     boot 2 判据失败 → 停喂 → 复位
 *     boot 3 ...                              无限循环
 *
 *   板端实测每次复位到重新可连约 28 秒（完整记录见
 *   openspec/tasks/2026-11-14-system-health-watchdog-plan-b.md §6.4）。
 *   每 28 秒重置一次的网关等于永久离线，而且**远程隧道来不及建连**，
 *   人永远进不去修 —— 产品上这叫「必须派人到现场」。
 *
 * 为什么用「连续启动次数」而不是时间窗：
 *   本板**没有 RTC**，开机初期系统时间停在 1970 附近，NTP 校正会跳变。
 *   任何「最近 N 小时内触发过几次」的时间窗都不可靠。boot_id 不依赖时钟。
 *
 * 文件格式：每行 `<boot_id> <reason> <epoch_or_0>`，追加写、保留最后若干行。
 * 用纯文本而不是 JSON：解析器只有十几行，不值得为它引 JSON 依赖
 * （本文件刻意不链接任何库，见 client/CMakeLists.txt 的说明）。
 */
constexpr const char* kTripHistory = "/var/lib/weaknet/health-guard-trip-history";

/*
 * 熔断进入标记。与失败 JSON 分开，语义不同：
 * 后者是「本次判据为什么失败」，前者是「保护已被主动关闭，需要人来处理」。
 * 运维在板上第一眼就该看到这个文件，所以单独一份、单独一个名字。
 */
constexpr const char* kRescueJson = "/var/lib/weaknet/health-guard-rescue.json";

/*
 * 保留的历史行数。够判定阈值 + 余量即可，不必无限增长。
 * 5 行 = 阈值 3 + 2 行余量（人工排障时能看到更早的两次）。
 */
constexpr int kTripHistoryMax = 5;

/*
 * 连续多少个不同 boot 都以「触发」收场才熔断。
 *
 * 3 的理由：一次真实故障（内存瞬时耗尽、磁盘临时写满）复位后自愈，
 *   不构成确定性故障；连续三次说明复位无法修复它。
 *   不能取 2：单次偶发 + 一次真实故障就被判死，代价太大。
 */
constexpr int kTripBreakerThreshold = 3;

/*
 * 熔断后的退出码。与 kExitTripped(99) 并列，同样必须在
 * systemd 的 RestartPreventExitStatus 里，否则会被重新拉起。
 *
 * 为什么单独一个码而不是复用 99：运维看到 98 就知道「不是判据失败，
 * 是熔断器判定这台设备处于确定性故障」，处置完全不同（前者查服务，
 * 后者查设备本身）。
 */
constexpr int kExitBreakerOpen = 98;

/*
 * 健康运行多久才清空触发历史。
 *
 * 为什么需要：没有它，一台设备跑几年、期间偶发两三次真实故障，
 * 历史会永久累积，第四次就直接熔断 —— 那等于把「偶发」当成了「确定性」。
 *
 * 30 分钟：远超一次正常启动 + 判据稳定所需时间（启动豁免 12 秒 +
 * 去抖 4 秒），又短到足以在一次人工巡检窗口内完成清空。
 */
constexpr int kTripHistoryResetSec = 30 * 60;

/*
 * 墙钟跳变判定阈值（秒）。
 *
 * 每轮比较「墙钟增量」与「单调钟增量」，两者不符即判定本轮机钟跳变。
 * 一轮 kTickSeconds=2，正常情况下两个增量都约等于 2，差值接近 0；
 * 取 5 秒是为了吸收 sleep 抖动与判据执行耗时，同时远小于任何一次
 * NTP 校正幅度（板端实测一次校正前跳 9785 秒）。
 */
constexpr int kClockStepThresholdSec = 5;

/*
 * 连续墙钟跳变的容忍轮数上限。
 *
 * 豁免本身是有代价的：做豁免的那一轮，所有基于墙钟的判据都不生效。
 * 若时钟**每一轮**都在跳（例如有人手工反复 `date`、或 timesyncd 陷入
 * 反复校正），无限豁免就等于把判据永久关掉 —— 保护在，判据不在。
 *
 * 3 轮 = 6 秒。正常的 NTP 校正是**一次性**的，跳完下一轮就恢复，
 * 因此 3 轮足够覆盖「跳变 + 稳定」；超过 3 轮说明时钟本身不可信，
 * 这时应当恢复判定（让判据的失败被看见），而不是继续装作健康。
 */
constexpr int kClockStepMaxStreak = 3;

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

int64_t MonotonicNs()
{
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

int64_t WallClockSeconds()
{
    struct timespec ts {};
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<int64_t>(ts.tv_sec);
}

/*
 * 墙钟跳变检测器。
 *
 * 为什么必须有：
 *   本板**没有 RTC 电池**，开机后墙钟先由 systemd-timesyncd 用「记录时间戳」
 *   恢复，再等 NTP 真正同步。同步完成的那一瞬间，墙钟会一次性前跳（板端
 *   2026-11-21 实测前跳 9785 秒）。凡是用「当前墙钟 − 文件 mtime」算年龄的
 *   判据，都会把这次跳变幅度当成「文件已经陈旧了 9785 秒」。
 *
 *   受影响的判定不止一处，而且是三处语义完全不同的地方：
 *     1) wal_mtime   → 假失败，去抖两轮后停喂 → 整机复位；
 *     2) 维护标记    → 被误判为 EXPIRED，维护模式静默失效，
 *                       此时一次 systemctl restart 就会变成整机复位；
 *     3) OTA 回执    → 被误判为 STALLED，恢复判定；若升级正停在
 *                       服务已停的阶段，板子会被复位在升级中途 —— 比升级
 *                       失败更糟（可能落在指针已翻转、release 未转正的位置）。
 *
 *   三处都无法靠「放大阈值」解决：跳变幅度可达数小时，任何合理阈值都会被
 *   穿透；而把阈值放大到几小时又等于废掉判据本身。
 *
 * 做法：
 *   单调钟（CLOCK_MONOTONIC）不受墙钟校正影响，因此
 *       step = 墙钟增量 − 单调钟增量
 *   就是本轮发生的墙钟跳变幅度。绝对值超过 kClockStepThresholdSec 时，
 *   **本轮所有基于墙钟年龄的判定都不可信**，直接豁免、不去猜文件真实年龄；
 *   下一轮墙钟已经稳定，年龄恢复正常。
 *
 * 为什么是「豁免一轮」而不是「换算回真实年龄」：
 *   换算需要知道每个 mtime 是在跳变前还是跳变后写入的，而那需要为每个
 *   文件维护影子时间戳 —— 引入的状态越多，出错面越大。豁免一轮的代价是
 *   最多 2 秒的判定延迟，而 NTP 同步是开机阶段的一次性事件，不是常态。
 *
 * 上限 kClockStepMaxStreak 的理由见该常量：防止「每轮都跳」把保护永久关掉。
 */
struct ClockStepDetector {
    int64_t last_wall = 0;
    int64_t last_mono = 0;
    int     streak    = 0;   /* 连续跳变轮数 */
};

/** 返回本轮墙钟跳变秒数；0 表示正常。首轮只建立基线。 */
long DetectClockStep(ClockStepDetector& d)
{
    int64_t wall = WallClockSeconds();
    int64_t mono = MonotonicNs() / 1000000000LL;

    if (d.last_mono == 0 && d.last_wall == 0) {
        d.last_wall = wall;
        d.last_mono = mono;
        return 0;
    }

    long step = static_cast<long>((wall - d.last_wall) - (mono - d.last_mono));
    d.last_wall = wall;
    d.last_mono = mono;

    if (step > kClockStepThresholdSec || step < -kClockStepThresholdSec) {
        ++d.streak;
        return step;
    }
    d.streak = 0;
    return 0;
}


/*
 * 执行命令并整段读取 stdout，**带硬超时**。调用方负责 free(buf)。
 *
 * 为什么不能用 popen/pclose（历史实现，2026-11-16 替换）：
 *   pclose 会一直等到子进程退出。子进程挂死（D-Bus daemon 卡住、
 *   systemd 卡住）时主循环永久停在 pclose 上 —— 不再喂狗，硬件超时复位，
 *   而 WriteFailureJson 永远执行不到。见 kCmdTimeoutMs 的完整推导。
 *
 * 另一条同样致命的老缺陷：fread 只读 cap-1 字节就返回，随后 pclose 等
 *   子进程退出，而子进程正阻塞在「写一个已满的管道」上 —— 输出超过
 *   缓冲区时同样死锁。本实现改成「缓冲满后继续 drain 到丢弃缓冲」，
 *   保证截断但不死锁。
 *
 * 语义约定：
 *   返回非 nullptr 时 *out_rc 为：子进程退出码（正常结束），或 -ETIMEDOUT。
 *   返回 nullptr 时 *out_rc 为 -errno（fork/pipe/malloc 失败）。
 *   **超时也返回已读到的部分**（可能为空串），让调用方能把 timeout
 *   当成一次「本轮判据失败」来记录，而不是把已有输出一并丢掉。
 *
 * 超时用 SIGKILL 杀**整个进程组**，不杀单个 pid：/bin/sh -c 会 fork
 * 出真正的命令，只杀 sh 会留下孤儿继续占管道，waitpid 照样收不到 EOF。
 */
char* RunCommand(const char* cmd, size_t cap, int timeout_ms, int* out_rc)
{
    if (out_rc) *out_rc = -EINVAL;
    if (cap < 2 || !cmd) {
        return nullptr;
    }

    char* buf = static_cast<char*>(malloc(cap));
    if (!buf) {
        if (out_rc) *out_rc = -ENOMEM;
        return nullptr;
    }
    buf[0] = '\0';

    int fds[2];
    /* CLOEXEC：读端不能泄漏给子进程，否则我们死了子进程还持着管道 */
    if (::pipe2(fds, O_CLOEXEC) != 0) {
        int e = errno;
        free(buf);
        if (out_rc) *out_rc = -e;
        return nullptr;
    }

    pid_t pid = ::fork();
    if (pid < 0) {
        int e = errno;
        ::close(fds[0]);
        ::close(fds[1]);
        free(buf);
        if (out_rc) *out_rc = -e;
        return nullptr;
    }

    if (pid == 0) {
        /*
         * 子进程：自成一个进程组，便于父进程一次杀干净 sh 及其后代。
         * 父进程也会做同样的 setpgid，两边都设是为了消除「子进程还没
         * 跑到这里就被 kill(-pid)」的竞态（谁先设上都不影响结果）。
         */
        ::setpgid(0, 0);
        ::dup2(fds[1], STDOUT_FILENO);
        ::dup2(fds[1], STDERR_FILENO);   /* 调用方的 cmd 已带 2>/dev/null */
        ::close(fds[0]);
        ::close(fds[1]);
        ::execl("/bin/sh", "sh", "-c", cmd, static_cast<char*>(nullptr));
        ::_exit(127);
    }

    ::setpgid(pid, pid);
    ::close(fds[1]);

    const int64_t deadline = MonotonicNs() +
                             static_cast<int64_t>(timeout_ms) * 1000000LL;

    size_t total = 0;
    bool timed_out = false;
    bool eof = false;

    /*
     * 读取阶段：缓冲未满时写入 buf，满了以后丢进一个小型 discard 缓冲。
     * 两种情况都必须读到 EOF 或超时，不能「读满就撒手」——那会死锁。
     */
    char discard[4096];
    while (!eof) {
        int64_t remain_ns = deadline - MonotonicNs();
        if (remain_ns <= 0) {
            timed_out = true;
            break;
        }
        int remain_ms = static_cast<int>(remain_ns / 1000000LL);
        if (remain_ms < 1) {
            remain_ms = 1;   /* poll 的 0 表示「立即返回」，不是「等到死线」 */
        }

        struct pollfd pfd {};
        pfd.fd = fds[0];
        pfd.events = POLLIN;
        int pr = ::poll(&pfd, 1, remain_ms);
        if (pr < 0) {
            if (errno == EINTR) {
                if (g_stop_requested) {
                    break;   /* 收到 SIGTERM：交给收尾逻辑统一处理 */
                }
                continue;
            }
            break;
        }
        if (pr == 0) {
            timed_out = true;    /* 到死线了 */
            break;
        }

        char* dst = (total < cap - 1) ? (buf + total) : discard;
        size_t dst_cap = (total < cap - 1) ? (cap - 1 - total) : sizeof(discard);
        ssize_t n = ::read(fds[0], dst, dst_cap);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (n == 0) {
            eof = true;
            break;
        }
        if (dst == buf) {
            total += static_cast<size_t>(n);
        }
    }
    buf[total] = '\0';
    ::close(fds[0]);

    if (!eof) {
        /*
         * 超时或读错误：杀整个进程组并回收，避免僵尸与孤儿。
         * kill(-pid) 要求 pid 已是组长；父进程已 setpgid，正常都成立。
         * 万一失败（子进程还没设组就死了），退回按 pid 杀。
         */
        if (::kill(-pid, SIGKILL) != 0) {
            ::kill(pid, SIGKILL);
        }
        timed_out = true;
    }

    /* 回收：无论正常还是被杀都要 waitpid，否则留僵尸 */
    int status = 0;
    for (;;) {
        pid_t w = ::waitpid(pid, &status, 0);
        if (w == pid) {
            break;
        }
        if (w < 0 && errno == EINTR) {
            continue;
        }
        break;
    }

    if (timed_out) {
        if (out_rc) *out_rc = -ETIMEDOUT;
        return buf;
    }
    if (out_rc) {
        *out_rc = WIFEXITED(status) ? WEXITSTATUS(status) : -ECHILD;
    }
    return buf;
}

/* ---- 判据 1：落库推进 ---- */

/*
 * 墙钟跳变豁免。三处判据（WAL 年龄 / 维护标记 / OTA 回执）共用同一个
 * 语义：本轮不能用墙钟推年龄。统一走这一个函数，避免各写各的日志。
 *
 * 返回 true 表示「本轮已豁免」，调用方直接按通过处理。
 */
bool ClockStepExempt(long step, int streak, const char* who)
{
    if (step == 0) {
        return false;
    }
    if (streak > kClockStepMaxStreak) {
        /*
         * 连续跳变超过上限：不再豁免。此时判定结果**不可信**，
         * 但继续豁免等于永久关掉保护，两害相权取「让失败被看见」。
         * 硬件复位后时钟从头再跳一次，仍会走正常的单轮豁免。
         */
        fprintf(stderr,
                "CLOCK_STEP %s step=%lds streak=%d (> %d) -> exemption exhausted,"
                " judgement resumes (results may be unreliable)\n",
                who, step, streak, kClockStepMaxStreak);
        return false;
    }
    fprintf(stderr,
            "CLOCK_STEP %s step=%lds streak=%d/%d -> exempt this round"
            " (wall clock jumped; mtime age is meaningless)\n",
            who, step, streak, kClockStepMaxStreak);
    return true;
}

bool CheckWalMtime(CheckState& st, long clock_step, int clock_streak)
{
    struct stat sb {};
    if (::stat(kWalPath, &sb) != 0) {
        snprintf(st.detail, sizeof(st.detail), "stat failed errno=%d", errno);
        return false;
    }
    if (ClockStepExempt(clock_step, clock_streak, "wal_mtime")) {
        snprintf(st.detail, sizeof(st.detail),
                 "clock_step=%lds, age unknown (exempt)", clock_step);
        return true;
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
    char* buf = RunCommand(kCmd, 64 * 1024, kCmdTimeoutMs, &rc);
    if (!buf) {
        snprintf(st.detail, sizeof(st.detail), "spawn failed rc=%d", rc);
        return false;
    }
    if (rc == -ETIMEDOUT) {
        /*
         * 超时算失败，交给 kDebounceK 吸收（单次抖动下一轮归零）。
         * 不能算通过：判据在「无法判定」时必须按不健康处理，
         * 否则一次挂死就能让保护永久静默。
         *
         * 也不能和 busctl 自身报错混成一个 detail —— 前者说明 D-Bus
         * 或本进程的调度出了问题，后者说明服务端拒绝/超时，处置不同。
         */
        snprintf(st.detail, sizeof(st.detail), "timeout %dms", kCmdTimeoutMs);
        free(buf);
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

/**
 * 用 systemd 取 MainPID。返回 0 表示拿不到。
 *
 * out_unobservable 用于把「观测失败」与「服务确实不在」分开：
 *   systemctl 超时（systemd 自身卡住）不是「服务死了」的证据 ——
 *   我们只是不知道。若把超时当成 pid<=0 处理，一次 systemd 抖动就会
 *   走「服务缺席 12 秒 → 停喂 → 整机复位」那条路，而这正是守卫
 *   把自己的故障伪装成被监控对象故障的第二种形态（第一种见 RunCommand）。
 *
 * 调用方据此决定：可观测失败按「无法判定」处理（见 main 里的
 * pid_unknown_streak），真正拿不到 PID 才按服务缺席处理。
 */
pid_t GetMainPid(bool* out_unobservable)
{
    if (out_unobservable) *out_unobservable = false;

    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "systemctl show -p MainPID --value %s 2>/dev/null", kServiceName);
    int rc = 0;
    char* buf = RunCommand(cmd, 64, kCmdTimeoutMs, &rc);
    if (!buf) {
        if (out_unobservable && rc == -ETIMEDOUT) *out_unobservable = true;
        return 0;
    }
    if (rc == -ETIMEDOUT) {
        free(buf);
        if (out_unobservable) *out_unobservable = true;
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

/* ---- 磁盘取证（只观测，不参与判定） ---- */

/*
 * 为什么磁盘水位**不是**第五项判据。
 *
 * 初稿曾打算「剩余空间低于阈值即判不健康」，推演后否决：
 *   磁盘满是一个**复位无法修复**的条件。加进判据 = 磁盘满 → 停喂 →
 *   复位 → 磁盘还是满 → 再停喂 → 复位。这是用保护机制制造伤害。
 *
 *   而且这个洞已经堵上了：磁盘满导致 SQLite 写不进去 → history.db-wal
 *   的 mtime 停止推进 → CheckWalMtime（kWalMaxAgeSec = 12）失败 →
 *   判据 1 自然触发复位。再加一项只是把同一个事实数两遍。
 *
 * 那它还有什么用？两条，都是**取证**价值：
 *
 *   1) 归因。复位后运维只看得到 "wal_mtime age_s=15"，看不出是磁盘写满
 *      还是落库线程卡死。把 free_pct 一起写进失败快照，一眼就能分开。
 *
 *   2) 反向暴露证据丢失。/var/lib/weaknet（放 trip flag 与失败 JSON）与
 *      /home/radxa/weaknet/data（放 WAL）**可能不在同一个文件系统上**。
 *      根分区写满时 RecordTrip() 与 WriteFailureJson() 会静默失败 ——
 *      复位后没有任何记忆，而且没人知道。这正是「保护看起来在、实际
 *      不在」最隐蔽的形态，必须让它可见。
 */
struct DiskWatermark {
    const char*   path;
    unsigned long free_pct;      /* 可用 / 总量 × 100，四舍五入 */
    unsigned long long avail_mb; /* f_bavail × f_frsize / 1MiB */
    bool          ok;            /* statvfs 是否成功 */
};

/*
 * 低于这个百分比就在每轮日志里喊 DISK_LOW。
 *
 * 10% 的理由：本仓库历史库 192 MB 量级、落库间隔 5 秒。留 10% 意味着
 * 还有十几个小时的人工介入窗口，足够一次远程排查；低于 5% 往往只剩
 * 几十分钟，滚动日志本身就会把余量吃光。
 *
 * 这个阈值**只触发日志**，不改变喂狗行为 —— 见上面的否决理由。
 */
constexpr unsigned long kDiskWarnPct = 10;

/*
 * 采样一个路径所在文件系统的水位。
 *
 * 用 statvfs 的 f_bavail（非特权用户可用块）而不是 f_bfree：
 * 后者含保留块，对「我这进程还写不写得进去」没有意义。
 * guard 以 root 跑，但按最保守口径报告总是更安全。
 */
void SampleDisk(const char* path, DiskWatermark* out)
{
    out->path = path;
    out->free_pct = 0;
    out->avail_mb = 0;
    out->ok = false;

    struct statvfs vfs {};
    if (::statvfs(path, &vfs) != 0) {
        return;
    }
    if (vfs.f_blocks == 0 || vfs.f_frsize == 0) {
        return;
    }
    out->ok = true;

    const unsigned long long frsize = vfs.f_frsize;
    const unsigned long long total = static_cast<unsigned long long>(vfs.f_blocks) * frsize;
    const unsigned long long avail = static_cast<unsigned long long>(vfs.f_bavail) * frsize;
    out->avail_mb = avail / (1024ULL * 1024ULL);
    /* 四舍五入，避免 9.6% 显示成 9 还是 10 看实现 */
    out->free_pct = total
        ? static_cast<unsigned long>((avail * 100ULL + total / 2) / total)
        : 0;
}

/*
 * 两条路径必须分开采样：它们常常落在不同分区上（见上面第 2 条价值）。
 * 失败 JSON 与日志都同时报两个，不做「取较小值」这种合并 ——
 * 合并会正好把「哪个分区满了」这个关键信息抹掉。
 */
void SampleBothDisks(DiskWatermark* ev, DiskWatermark* data)
{
    SampleDisk(kFailureDir, ev);
    SampleDisk(kDataDir, data);
}

void LogDisk(DiskWatermark& ev, DiskWatermark& data)
{
    unsigned long min_pct = 100;
    if (ev.ok && ev.free_pct < min_pct) min_pct = ev.free_pct;
    if (data.ok && data.free_pct < min_pct) min_pct = data.free_pct;

    fprintf(stderr, " disk[evidence=");
    if (ev.ok) {
        fprintf(stderr, "%lu%%/%lluMB", ev.free_pct, ev.avail_mb);
    } else {
        fprintf(stderr, "STAT_FAILED");
    }
    fprintf(stderr, " data=");
    if (data.ok) {
        fprintf(stderr, "%lu%%/%lluMB", data.free_pct, data.avail_mb);
    } else {
        fprintf(stderr, "STAT_FAILED");
    }
    fprintf(stderr, "]%s", (min_pct < kDiskWarnPct) ? " DISK_LOW" : "");
}

/* ---- 失败 JSON ---- */

/* 定义见下方「维护与触发标记」一节；这里前置声明以便复用 */
void ReadBootId(char* buf, size_t len);

/* 定义见下方「连续复位熔断」一节；RecordTrip 里要调用 */
void AppendTripHistory(const char* reason);

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
        /*
         * 原本静默 return。快照写不出来的直接后果是「复位了但不知道为什么」,
         * 而最常见的原因就是证据分区写满 —— 所以先把磁盘水位喊出来。
         */
        DiskWatermark ev{}, data{};
        SampleBothDisks(&ev, &data);
        LogDisk(ev, data);
        fprintf(stderr,
                " WriteFailureJson FAILED: %s (no post-mortem snapshot; "
                "reason=%s pid=%d)\\n",
                strerror(errno), reason, static_cast<int>(pid));
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

    /*
     * 磁盘水位随快照一起落盘（只观测，不参与判定，理由见 DiskWatermark）。
     * 在**写 JSON 之前**采样：这次采样本身也要占几个字节，虽然可以忽略，
     * 但顺序上先采样更符合「看到的就是写这一份时的水位」。
     */
    DiskWatermark disk_ev{}, disk_data{};
    SampleBothDisks(&disk_ev, &disk_data);

    char out[2560];
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
        "  \"disk\": {\n"
        "    \"evidence_ok\": %s, \"evidence_free_pct\": %lu,"
        " \"evidence_avail_mb\": %llu,\n"
        "    \"data_ok\": %s, \"data_free_pct\": %lu,"
        " \"data_avail_mb\": %llu,\n"
        "    \"warn_pct\": %lu\n"
        "  },\n"
        "  \"action\": \"stopped_petting\",\n"
        "  \"hardware_timeout_s\": 16\n"
        "}\n",
        ts, uptime, boot_id, static_cast<int>(pid), reason,
        g_checks[0].ok ? "true" : "false", g_checks[0].detail,
        g_checks[0].consecutive_fail,
        g_checks[1].ok ? "true" : "false", g_checks[1].detail,
        g_checks[1].consecutive_fail,
        g_checks[2].ok ? "true" : "false", g_checks[2].detail,
        g_checks[2].consecutive_fail,
        g_checks[3].ok ? "true" : "false", g_checks[3].detail,
        g_checks[3].consecutive_fail,
        disk_ev.ok ? "true" : "false", disk_ev.free_pct, disk_ev.avail_mb,
        disk_data.ok ? "true" : "false", disk_data.free_pct,
        disk_data.avail_mb,
        kDiskWarnPct);

    if (n > 0) {
        ssize_t w = ::write(fd, out, static_cast<size_t>(n));
        (void)w;
        /* fsync 只保证内核->设备；write back 下仍可能丢，这是已知边界 */
        ::fsync(fd);
    }
    ::close(fd);
}

/* ---- 主逻辑 ---- */

/*
 * 设备契约断言：这条路线把全部硬件操作交给平台驱动，因此必须在使用前
 * 确认驱动仍然提供我们依赖的那几条语义。任一不满足就拒绝启动 ——
 * 宁可没有保护，也不要一个「看起来有、实际没有」的保护。
 *
 * 断言项与它们各自的失效后果：
 *
 *   1) WDIOF_MAGICCLOSE
 *      必须支持。判据失败时我们不写 'V' 直接 close；watchdog_release()
 *      里只有「收到 'V'」或「驱动不支持 MAGICCLOSE」才走 watchdog_stop()。
 *      驱动一旦不支持 MAGICCLOSE，那句 else-if 就恒为真，于是不写 'V'
 *      也会停狗 —— 停止喂狗不再导致复位，整个判据归零。
 *
 *   2) 超时 >= 去抖窗口的 2 倍
 *      判据确认需要 kTickSeconds * kDebounceK = 4 秒，硬件超时必须显著
 *      大于它，否则还没确认完板子就复位了。读实际值而不是写死 16：
 *      平台驱动或 DT 的 timeout-sec 都可能改变它。
 *
 *   3) 可写（以 O_WRONLY 打开成功）
 *      写不进去就无法喂狗，等于立即触发复位。open 本身已经验证了这条。
 *
 * 不读 WDIOC_GETBOOTSTATUS：sunxi-wdt 从不设 bootstatus（恒 0x0），
 * 该位没有任何信息量，读它只会制造「我有依据」的错觉。
 */
bool VerifyDeviceContract(int fd, char* out_reason, size_t len)
{
    struct watchdog_info wi {};
    if (::ioctl(fd, WDIOC_GETSUPPORT, &wi) != 0) {
        snprintf(out_reason, len, "WDIOC_GETSUPPORT failed: %s", strerror(errno));
        return false;
    }

    /* identity 是驱动自报的名字；只用于日志，不参与判定 */
    wi.identity[sizeof(wi.identity) - 1] = '\0';

    if ((wi.options & WDIOF_MAGICCLOSE) == 0) {
        /*
         * 这是最危险的一种退化：设备能开、能喂、systemctl status 正常，
         * 只有「停止喂狗」这一个动作失效。不硬断言就只能在故障当天才发现。
         */
        snprintf(out_reason, len,
                 "driver %s 不支持 WDIOF_MAGICCLOSE（options=0x%04x）："
                 "close 不写 'V' 也会停狗，停止喂狗无法触发复位",
                 wi.identity, wi.options);
        return false;
    }

    int timeout = 0;
    if (::ioctl(fd, WDIOC_GETTIMEOUT, &timeout) != 0) {
        snprintf(out_reason, len, "WDIOC_GETTIMEOUT failed: %s", strerror(errno));
        return false;
    }
    const int need = kTickSeconds * kDebounceK * 2;
    if (timeout < need) {
        snprintf(out_reason, len,
                 "硬件超时 %ds 小于安全下限 %ds（去抖 %ds 的两倍）",
                 timeout, need, kTickSeconds * kDebounceK);
        return false;
    }

    fprintf(stderr,
            "watchdog contract ok: identity=%s options=0x%04x timeout=%ds\n",
            wi.identity, wi.options, timeout);
    return true;
}

int OpenWatchdog()
{
    /*
     * 两个候选节点指向同一个 watchdog_device，先 misc 后动态编号。
     * 两者都不通才算失败：EBUSY 要原样回报，因为「设备被别的进程占着」
     * 和「设备不存在」的处置完全不同（前者要人介入，后者多半是驱动没起）。
     */
    int fd = ::open(kWatchdogDev, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        int first_errno = errno;
        fd = ::open(kWatchdogAlt, O_WRONLY | O_CLOEXEC);
        if (fd < 0) {
            /*
             * 两个都失败时优先报告 EBUSY：如果第一个是 EBUSY，那才是
             * 真实原因（有人在用），第二个的 ENOENT 只是噪声。
             */
            if (first_errno != EBUSY) {
                /* 保留第二个 errno，调用方据此打印 */
            } else {
                errno = first_errno;
            }
            return -1;
        }
    }
    return fd;
}

/*
 * 维护标记是否有效（人工显式创建，见 kMaintFlag / kMaintMaxAgeSec 注释）。
 *
 * 不看 mtime 是绝对错误的：标记一旦因 Ctrl-C 残留，保护就永久消失。
 * 过期时在 out_reason 里说明原因，由调用方打进日志。
 */
bool MaintenanceRequested(char* out_reason, size_t len,
                         long clock_step, int clock_streak)
{
    struct stat sb {};
    if (::stat(kMaintFlag, &sb) != 0) {
        snprintf(out_reason, len, "absent");
        return false;
    }
    /*
     * 墙钟跳变时年龄不可信。这里的方向必须是「按新鲜处理」：
     * 维护模式的价值是「人在现场、要它别复位」，误判为过期会把
     * 一次正常维护变成整机复位；而误判为新鲜的代价只是保护多暂停一轮。
     */
    if (ClockStepExempt(clock_step, clock_streak, "maintenance")) {
        snprintf(out_reason, len, "present (age unknown, clock_step=%lds)",
                 clock_step);
        return true;
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
bool OtaUpgradeInProgress(char* out_detail, size_t len,
                          long clock_step, int clock_streak)
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

    /*
     * 墙钟跳变时年龄不可信，方向同样取「按新鲜处理」：
     * 把一次仍在进行的升级误判成 STALLED，会让守卫恢复判定；若此时
     * 服务正处在「已 stop、尚未 start」的阶段，12 秒宽限一过就停喂，
     * 板子被复位在升级中途。误判为新鲜的代价只是判据多暂停一轮。
     */
    if (ClockStepExempt(clock_step, clock_streak, "ota_receipt")) {
        snprintf(out_detail, len,
                 "IN_PROGRESS (age unknown, clock_step=%lds, exempt)",
                 clock_step);
        return true;
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

/**
 * 记录本次启动已触发。失败不致命：本进程仍会停止喂狗。
 *
 * 同时把触发原因追加进跨 boot 历史（供熔断器使用）。两件事放在同一个
 * 函数里，是因为它们必须成对发生：只写 trip flag 不写历史 → 熔断器
 * 永远看不到确定性故障；只写历史不写 trip flag → 重启能撤销复位。
 * 分成两个调用点让调用方各写一次，是最容易漏掉其中一个的写法。
 *
 * 已知边界：若 trip flag 写成功而历史写失败（例如两个路径在不同
 * 分区、其中一个满了），熔断器会少看一次触发。这不是安全问题
 * （少一次熔断只是保护弱一点），但会让阈值判定偏晚，AppendTripHistory
 * 里已经会喊出失败原因。
 */
void RecordTrip(const char* reason)
{
    ::mkdir(kFailureDir, 0755);
    int fd = ::open(kTripFlag, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        /*
         * 这里原本是静默 return。改成喊出来，因为它的后果非常隐蔽：
         * trip flag 没写下 = 复位后这个 boot 没有任何记忆 = 守卫被
         * systemd 重新拉起后会再次 open 设备，把上一次判定的复位
         * **静默取消**。系统看起来有保护，实际永远等不到复位。
         *
         * 顺带把磁盘水位一起报出来 —— 这个 open 失败最常见的原因就是
         * 根分区写满，而 99% 的排障者不会想到去查磁盘。
         */
        DiskWatermark ev{}, data{};
        SampleBothDisks(&ev, &data);
        LogDisk(ev, data);
        fprintf(stderr,
                " RecordTrip FAILED: %s (trip flag LOST for boot; "
                "a restart would silently cancel the scheduled reset)\n",
                strerror(errno));
        return;
    }
    char bid[64];
    ReadBootId(bid, sizeof(bid));

    /*
     * trip flag 保持「只有一个 boot_id」的极简格式：
     * 它的唯一用途是 AlreadyTrippedThisBoot() 的等值比较，不需要更多信息。
     * 带 reason 的历史在 kTripHistory 里，两份职责分开。
     */
    char line[80];
    int n = snprintf(line, sizeof(line), "%s\n", bid);
    if (n > 0) {
        ssize_t w = ::write(fd, line, static_cast<size_t>(n));
        (void)w;
        ::fsync(fd);
    }
    ::close(fd);

    /* 定义在下方「连续复位熔断」一节；前置声明避免把两节搅在一起 */
    AppendTripHistory(reason);
}

/* ---- 连续复位熔断 ---- */

/**
 * 追加一条触发记录，只保留最后 kTripHistoryMax 行。
 *
 * 读取 → 追加 → 截断重写，全程不用 flock：guard 是单实例的
 * （设备本身 single-open，第二个实例连 watchdog 都打不开，
 * 会在 open 阶段就退出），因此不存在并发写这个文件的问题。
 *
 * 用 O_TRUNC 重写而不是 append：需要真的裁掉老行。文件只有几百字节，
 * 重写的代价可以忽略，而「文件不会无限增长」比省一次 write 重要得多。
 */
void AppendTripHistory(const char* reason)
{
    ::mkdir(kFailureDir, 0755);

    char bid[64];
    ReadBootId(bid, sizeof(bid));

    time_t now = ::time(nullptr);
    /* 无 RTC 时 time() 可能是 1970；记 0 让读的人知道「这个时间戳无意义」 */
    long long epoch = (now > 1600000000) ? static_cast<long long>(now) : 0;

    /*
     * 读出现有行。文件最大 5 行 × 约 60 字节，1KB 足够；
     * 万一被外部写大了，按整块读入再按行裁，不丢最新记录。
     */
    char old[4096];
    old[0] = '\0';
    ReadFile(kTripHistory, old, sizeof(old));

    /* 按行收集（保留最后 kTripHistoryMax-1 行，再追加本次） */
    char lines[kTripHistoryMax][128];
    int cnt = 0;
    const char* p = old;
    while (*p != '\0') {
        const char* nl = strchr(p, '\n');
        size_t len = nl ? static_cast<size_t>(nl - p) : strlen(p);
        if (len > 0 && len < sizeof(lines[0])) {
            if (cnt < kTripHistoryMax) {
                memcpy(lines[cnt], p, len);
                lines[cnt][len] = '\0';
                ++cnt;
            } else {
                /* 满了：整体前移一行，腾出末尾 */
                for (int i = 1; i < kTripHistoryMax; ++i) {
                    memcpy(lines[i - 1], lines[i], sizeof(lines[0]));
                }
                memcpy(lines[kTripHistoryMax - 1], p, len);
                lines[kTripHistoryMax - 1][len] = '\0';
            }
        }
        if (!nl) break;
        p = nl + 1;
    }

    /* 本次记录压入末尾（同样保持上限） */
    char cur[128];
    snprintf(cur, sizeof(cur), "%s %s %lld", bid, reason, epoch);
    if (cnt < kTripHistoryMax) {
        snprintf(lines[cnt], sizeof(lines[0]), "%s", cur);
        ++cnt;
    } else {
        for (int i = 1; i < kTripHistoryMax; ++i) {
            memcpy(lines[i - 1], lines[i], sizeof(lines[0]));
        }
        snprintf(lines[kTripHistoryMax - 1], sizeof(lines[0]), "%s", cur);
    }

    int fd = ::open(kTripHistory,
                    O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        /*
         * 写不进去不致命，但必须喊出来：没有历史 = 熔断器看不到这次触发，
         * 无限复位会继续。与 RecordTrip 同样把磁盘水位一起报出来。
         */
        DiskWatermark ev{}, data{};
        SampleBothDisks(&ev, &data);
        LogDisk(ev, data);
        fprintf(stderr,
                " AppendTripHistory FAILED: %s (breaker blind to this trip)\n",
                strerror(errno));
        return;
    }
    for (int i = 0; i < cnt; ++i) {
        char buf[140];
        int n = snprintf(buf, sizeof(buf), "%s\n", lines[i]);
        if (n > 0) {
            ssize_t w = ::write(fd, buf, static_cast<size_t>(n));
            (void)w;
        }
    }
    ::fsync(fd);
    ::close(fd);
}

/**
 * 熔断器是否应该打开。
 *
 * 判定：历史里最后 kTripBreakerThreshold 条的 boot_id
 *       **互不相同**且**都不是当前 boot** 时打开。
 *
 * 「都不是当前 boot」这一条是必须的：如果当前 boot 已经触发过一次，
 * 那说明本次启动还没失败 —— 它只是还没走完。把它算进去会让阈值
 * 提前一个 boot 触发。
 *
 * 返回 true 时把原因写进 out_reason 供日志与 rescue JSON 使用。
 */
bool BreakerOpen(char* out_reason, size_t len)
{
    char cur[64];
    ReadBootId(cur, sizeof(cur));

    char raw[4096];
    if (!ReadFile(kTripHistory, raw, sizeof(raw))) {
        snprintf(out_reason, len, "no history");
        return false;
    }

    /* 收集所有 boot_id */
    char ids[kTripHistoryMax][64];
    int cnt = 0;
    const char* p = raw;
    while (*p != '\0' && cnt < kTripHistoryMax) {
        const char* nl = strchr(p, '\n');
        size_t linelen = nl ? static_cast<size_t>(nl - p) : strlen(p);
        if (linelen > 0) {
            /* 第一段是 boot_id */
            size_t j = 0;
            while (j < linelen && p[j] != ' ' && j < sizeof(ids[0]) - 1) {
                ids[cnt][j] = p[j];
                ++j;
            }
            ids[cnt][j] = '\0';
            if (j > 0) ++cnt;
        }
        if (!nl) break;
        p = nl + 1;
    }

    if (cnt < kTripBreakerThreshold) {
        snprintf(out_reason, len, "history only %d/%d",
                 cnt, kTripBreakerThreshold);
        return false;
    }

    /*
     * 取最后 kTripBreakerThreshold 条，要求它们互不相同且不含当前 boot。
     * 从末尾往前扫，遇到当前 boot 就整体作废（说明本次启动还没成功）。
     */
    int start = cnt - kTripBreakerThreshold;
    for (int i = start; i < cnt; ++i) {
        if (strcmp(ids[i], cur) == 0) {
            snprintf(out_reason, len,
                     "current boot already in window (slot %d)", i - start);
            return false;
        }
        for (int j = start; j < i; ++j) {
            if (strcmp(ids[i], ids[j]) == 0) {
                snprintf(out_reason, len, "duplicate boot_id in window");
                return false;
            }
        }
    }

    snprintf(out_reason, len,
             "%d consecutive trips on distinct boots: %s..%s",
             kTripBreakerThreshold, ids[start], ids[cnt - 1]);
    return true;
}

/** 健康运行满 kTripHistoryResetSec 后清空历史（见该常量的注释）。 */
void ResetTripHistoryIfHealthy(long clock_step, int clock_streak)
{
    struct stat sb {};
    if (::stat(kTripHistory, &sb) != 0) {
        return;   /* 没有历史，无需清理 */
    }
    /*
     * 墙钟跳变时不清：方向取「不清」是对的 —— 清早了会把刚累积的
     * 真实复位历史抹掉，熔断阈值（连续 3 次）就永远攒不够。
     * 不清的代价只是历史多留一轮，无保护风险。
     */
    if (ClockStepExempt(clock_step, clock_streak, "trip_history")) {
        return;
    }
    time_t now = ::time(nullptr);
    long age = static_cast<long>(now - sb.st_mtime);
    if (age < 0) {
        /*
         * 时钟回跳（无 RTC 的板子第一次 NTP 校正）：不能把它当成
         * 「很旧」而清空 —— 那正好会让刚累积的历史被一次校正抹掉。
         * 保守地不清。
         */
        return;
    }
    if (age < kTripHistoryResetSec) {
        return;
    }
    if (::unlink(kTripHistory) == 0) {
        fprintf(stderr,
                "trip history cleared after %lds of healthy operation\n", age);
    }
}

/**
 * 写熔断进入记录。与失败 JSON 分开，因为语义不同：
 * 后者是「本次判据为什么失败」，前者是「保护已被主动关闭」。
 */
void WriteRescueJson(const char* reason)
{
    ::mkdir(kFailureDir, 0755);
    int fd = ::open(kRescueJson, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        fprintf(stderr, "WriteRescueJson FAILED: %s\n", strerror(errno));
        return;
    }
    char boot_id[64];
    ReadBootId(boot_id, sizeof(boot_id));

    char ts[64];
    time_t now = ::time(nullptr);
    struct tm tmv {};
    gmtime_r(&now, &tmv);
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tmv);

    char out[1024];
    int n = snprintf(out, sizeof(out),
        "{\n"
        "  \"entered_at\": \"%s\",\n"
        "  \"boot_id\": \"%s\",\n"
        "  \"reason\": \"%s\",\n"
        "  \"action\": \"breaker_open_no_watchdog\",\n"
        "  \"threshold\": %d,\n"
        "  \"recover_by\": \"remove /var/lib/weaknet/health-guard-trip-history "
        "and restart weaknet-health-guard\"\n"
        "}\n",
        ts, boot_id, reason, kTripBreakerThreshold);
    if (n > 0) {
        ssize_t w = ::write(fd, out, static_cast<size_t>(n));
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
    /*
     * 磁盘水位只进日志，不进判据（理由见 DiskWatermark 一节）。
     * 放在同一行末尾，是为了让「判据全绿但磁盘只剩 2%」这种组合
     * 在一行日志里就能看见 —— 它往往是下一次复位的预告。
     */
    DiskWatermark ev{}, data{};
    SampleBothDisks(&ev, &data);
    LogDisk(ev, data);

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

    /*
     * 连续复位熔断，同样必须在 open() 之前。
     *
     * 顺序理由与上面那条相同：open() 会 arm 硬件。熔断的语义是
     * 「主动放弃保护、让设备留在可远程访问的状态」，一旦开了设备，
     * 本进程意外退出反而会触发复位 —— 与熔断的目的正好相反。
     *
     * 这里只判定、不写任何东西就退出：rescue JSON 由 WriteRescueJson 写，
     * 它自己会创建目录。不写 trip flag —— 熔断不是「判据失败」，
     * 混淆两者会让下一次启动的 AlreadyTrippedThisBoot 误判。
     */
    if (!dry_run) {
        char brk_reason[256];
        if (BreakerOpen(brk_reason, sizeof(brk_reason))) {
            fprintf(stderr,
                    "连续复位熔断已打开：%s\n"
                    "本设备连续 %d 次以「判定不健康并复位」收场，"
                    "复位无法修复这个故障。\n"
                    "**本次不打开看门狗设备，设备无硬件保护但保持在线**，"
                    "请远程介入排查。\n"
                    "解除方式：删除 %s 后重启本服务。\n",
                    brk_reason, kTripBreakerThreshold, kTripHistory);
            WriteRescueJson(brk_reason);
            return kExitBreakerOpen;
        }
    }

    int wd_fd = -1;
    if (!dry_run) {
        wd_fd = OpenWatchdog();
        if (wd_fd < 0) {
            fprintf(stderr,
                    "无法打开看门狗设备（%s 或 %s）：%s\n",
                    kWatchdogDev, kWatchdogAlt, strerror(errno));
            return 1;
        }
        /*
         * 契约断言必须在首次喂狗之前完成。
         *
         * 顺序理由：open() 已经把硬件使能了（watchdog_start），此时若发现
         * 契约不满足，唯一的正确动作是「立刻写 'V' 关掉再退出」，而不是
         * 「什么都不做直接退出」—— 后者会让硬件开始倒数，把一个「配置
         * 不满足、明确不打算提供服务」的场景变成一次整机复位。
         *
         * 这里不会静默降级：断言失败必须让单元起不来，运维才会去看日志。
         */
        char reason[256];
        if (!VerifyDeviceContract(wd_fd, reason, sizeof(reason))) {
            fprintf(stderr, "看门狗设备契约不满足：%s\n", reason);
            if (::write(wd_fd, "V", 1) != 1) {
                fprintf(stderr, "契约失败后写 'V' 也失败：%s\n", strerror(errno));
            }
            ::close(wd_fd);
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
    int pid_unknown_streak = 0;      /* 见 kPidUnknownMaxStreak */
    ClockStepDetector clock_det;     /* 见 DetectClockStep */

    while (!g_stop_requested) {
        ++round;

        /*
         * 墙钟跳变检测必须放在**所有**依赖墙钟年龄的判定之前。
         *
         * 本板无 RTC 电池，开机后 timesyncd 先恢复记录时间戳、再等 NTP
         * 真正同步；同步完成那一瞬间墙钟一次性前跳（板端实测 9785 秒）。
         * 该跳变会同时污染 WAL 年龄、维护标记有效期、OTA 回执新鲜度三处
         * 判定，方向各不相同，后果从「假失败误复位」到「保护静默失效」。
         * 详见 DetectClockStep 的注释。
         */
        const long clock_step = DetectClockStep(clock_det);
        const int  clock_streak = clock_det.streak;

        /* 维护模式优先于一切判据：标记有效就只喂狗不做判定 */
        char maint_detail[128];
        if (MaintenanceRequested(maint_detail, sizeof(maint_detail),
                                 clock_step, clock_streak)) {
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

        bool pid_unobservable = false;
        pid_t pid = GetMainPid(&pid_unobservable);
        if (pid_unobservable) {
            /*
             * systemctl 超时 = 我们不知道服务还在不在，而不是「它没了」。
             * 见 kPidUnknownMaxStreak：有界容忍后按判据失败处理，
             * 绝不悄悄走「服务缺席」那条路径，也不无限喂狗。
             */
            ++pid_unknown_streak;
            if (pid_unknown_streak <= kPidUnknownMaxStreak) {
                fprintf(stderr,
                        "[round %d] mainpid UNOBSERVABLE (%d/%d): "
                        "MainPID query timed out -> keep feeding, "
                        "judgement suspended\n",
                        round, pid_unknown_streak, kPidUnknownMaxStreak);
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
            fprintf(stderr,
                    "[round %d] mainpid UNOBSERVABLE for %d rounds (> %d) "
                    "-> stop feeding, expect hardware reset\n",
                    round, pid_unknown_streak, kPidUnknownMaxStreak);
            if (!dry_run) {
                RecordTrip("pid_unobservable");
                WriteFailureJson(0, "pid_unobservable");
                ::close(wd_fd);
                return kExitTripped;
            }
            pid = 0;
        }
        if (pid <= 0) {
            pid_unknown_streak = 0;
            /*
             * 先看是不是受控过渡（OTA 正在推进）。
             * 升级期间服务必然缺席，且时长可达数分钟，任何固定宽限都不够。
             * 判据交给升级自己的进度刷新，见 kOtaProgressMaxAgeSec。
             */
            char ota_detail[128];
            if (OtaUpgradeInProgress(ota_detail, sizeof(ota_detail),
                                     clock_step, clock_streak)) {
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
                        "-> stop feeding, hardware watchdog will reset the board\n",
                        round, kServiceName, static_cast<long long>(gone_s),
                        kInactiveGraceSec);
                if (!dry_run) {
                    RecordTrip("service_absent");
                    WriteFailureJson(0, "service_absent");
                    ::close(wd_fd);   /* 不写 'V' → 拒绝停狗 → 硬件超时复位 */
                    return kExitTripped;
                }
            }
        } else {
            inactive_since_ns = 0;
            pid_unknown_streak = 0;   /* 观测恢复正常，容忍计数归零 */

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

            g_checks[0].ok = CheckWalMtime(g_checks[0], clock_step, clock_streak);
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
                        "hardware watchdog will reset the board\n", round);
                if (!dry_run) {
                    RecordTrip("criteria_unhealthy");
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
                /*
                 * 健康运行时清空触发历史（见 kTripHistoryResetSec）。
                 * 每 15 轮检查一次就够：阈值是 30 分钟，而一轮 2 秒，
                 * 15 轮 = 30 秒的检测粒度，误差对 30 分钟毫无意义。
                 * 不每轮 stat 是为了让这条路径的开销可以忽略。
                 */
                if ((round % 15) == 0) {
                    ResetTripHistoryIfHealthy(clock_step, clock_streak);
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
