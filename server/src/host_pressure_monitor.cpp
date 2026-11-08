/**
 * @file host_pressure_monitor.cpp
 * @brief 采集端自身 CPU 压力 / 软中断构成的监控器实现
 *
 * 数据路径（两条并行，互不依赖）：
 *   kmod/weaknet_cpu_stat.c（内核 hrtimer，1 s）
 *     → /dev/weaknet_cpu_stat（只读 mmap，seqlock 保护）
 *     → HostCpuPressureSampler::sample()（区间差值 → 加权比率）
 *     → MetricsRegistry::publishScope(MetricScope::HOST, CPU_BUSY_RATIO)
 *     → quality 线程 / 诊断归因读取
 *
 *   kmod/weaknet_softirq.c（内核 hrtimer，1 s）
 *     → /dev/weaknet_softirq（只读 mmap，seqlock 保护）
 *     → HostSoftirqShareSampler::sample()（区间差值 → NET_RX 构成比）
 *     → MetricsRegistry::publishScope(MetricScope::HOST, SOFTIRQ_NET_RX_SHARE)
 *     → quality 线程 / 诊断归因读取
 *
 * 为什么这里直接 include kmod/weaknet_kmod_abi.h：
 *   那个头文件的存在意义就是"内核模块与用户态共享同一份结构定义"
 *   （见其文件头声明）。如果在服务端另抄一份，一旦内核侧改动结构体，
 *   服务端会在毫无征兆的情况下读到错位数据——而这个驱动 30 分钟长稳测试
 *   专门验过 seqlock 不撕裂，恰恰说明"读到的是稳定但错误的字段"这种
 *   故障最难发现。共享定义让编译器直接报错。
 *
 * 【必须记住的尺寸陷阱】：两个快照结构体大小不同，各自必须用**自己的**
 * 类型算 map 长度。历史上真实踩过：用 wn_cpu_snapshot 的 sizeof 去映射
 * softirq，得到 24600 > g_map_size 24576，内核直接返 EINVAL。
 * weaknet_kmod_abi.h 里有专门一段注释记录此事。
 */

#include "host_pressure_monitor.hpp"

#include "server.hpp"
#include "logger.hpp"
#include "metrics/metrics_registry.hpp"
#include "metrics/metric_types.hpp"
#include "metrics/metric_sample.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// 内核与用户态共享的 ABI 定义（纯数据结构，不含内核专有头）
#include "../../kmod/weaknet_kmod_abi.h"

namespace weaknet_dbus {

namespace {

/// seqlock 重试上限。正常写入窗口只覆盖一次 memcpy（8 核约 700 字节），
/// 用户态撞上奇数代次的概率极低；给足重试次数是为了在极端抢占下不误报失败。
constexpr int kMaxSeqRetry = 1000;

/**
 * @brief 以 32 位 acquire 语义读 seq 的低半字。
 *
 * 为什么不直接读 64 位：内核写 seq 用的是 WRITE_ONCE（在 arm64 上 64 位
 * 写本身原子），但用户态通过 mmap 读到的是同一块普通内存页，理论上存在
 * 缓存别名。32 位访问在两条架构上都保证不撕裂，代价可以忽略。
 */
uint32_t loadSeq(const uint64_t* seq) {
    const volatile uint32_t* p = reinterpret_cast<const volatile uint32_t*>(seq);
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

std::chrono::milliseconds readIntervalMs(const ServerContext* ctx) {
    const uint32_t ms = ctx->cfg.host_pressure.interval_ms.load();
    // 与配置层区间校验保持一致；越界值说明配置绕过了白名单，回落默认值而非崩溃
    if (ms < 1000 || ms > 600000) return std::chrono::milliseconds(15000);
    return std::chrono::milliseconds(ms);
}

}  // namespace

struct HostCpuPressureSampler::Impl {
    /// 单 CPU 的时间累计（纳秒），字段与 struct wn_cpu_stat 一一对应。
    struct CpuTimes {
        uint32_t cpu_id{0};
        bool online{false};
        bool valid{false};
        uint64_t busy{0};
        uint64_t total{0};
    };

    std::vector<CpuTimes> prev;
    bool have_prev{false};

    /// 映射在首次成功打开后保持，避免每轮 open/mmap 的开销
    int fd{-1};
    void* map{MAP_FAILED};
    size_t map_size{0};
};

HostCpuPressureSampler::HostCpuPressureSampler(std::string dev_path)
    : dev_path_(std::move(dev_path)), impl_(std::make_unique<Impl>()) {}

HostCpuPressureSampler::~HostCpuPressureSampler() {
    if (impl_ && impl_->map != MAP_FAILED) {
        munmap(impl_->map, impl_->map_size);
    }
    if (impl_ && impl_->fd >= 0) {
        close(impl_->fd);
    }
}

HostCpuPressure HostCpuPressureSampler::sample() {
    HostCpuPressure out;

    // ---- 1. 打开设备 + 映射（懒加载，失败后每轮重试，支持模块后插）----
    if (impl_->fd < 0) {
        impl_->fd = ::open(dev_path_.c_str(), O_RDONLY | O_CLOEXEC);
        if (impl_->fd < 0) {
            // 分类交给纯函数（可单测，见 host_pressure_monitor.hpp）。
            // errno 必须在任何其它调用之前捕获：std::strerror 之后 errno
            // 可能被改写。
            const int open_errno = errno;
            out.reason = openErrnoToReason(open_errno);
            out.detail = std::string("open: ") + std::strerror(open_errno);
            return out;
        }
        impl_->map_size = sizeof(struct wn_cpu_snapshot);
        impl_->map = mmap(nullptr, impl_->map_size, PROT_READ, MAP_SHARED, impl_->fd, 0);
        if (impl_->map == MAP_FAILED) {
            out.reason = "mmap_failed";
            out.detail = std::string("mmap: ") + std::strerror(errno);
            close(impl_->fd);
            impl_->fd = -1;
            return out;
        }
    }

    // ---- 2. seqlock 读者：校验 → 拷贝 → 再校验 ----
    auto* snap = static_cast<struct wn_cpu_snapshot*>(impl_->map);
    struct wn_cpu_snapshot local;

    uint32_t s0 = 0, s1 = 0;
    bool ok = false;
    for (int tries = 0; tries < kMaxSeqRetry; ++tries) {
        s0 = loadSeq(&snap->hdr.seq);
        if (s0 & 1u) continue;  // 内核正在写入

        std::memcpy(&local, snap, sizeof(local));

        // 拷贝完成后再校验一次：只在拷贝前校验会拿到半新半旧的数据
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        s1 = loadSeq(&snap->hdr.seq);

        if (s0 == s1) {
            ok = true;
            break;
        }
    }
    if (!ok) {
        out.reason = "read_failed";
        return out;
    }

    // ---- 3. ABI 版本校验：布局不符时宁可不可用，也不能拿错位数据当结论 ----
    if (local.hdr.abi_version != WEAKNET_KMOD_ABI_VERSION) {
        out.reason = "abi_mismatch";
        return out;
    }

    if (local.hdr.nr_cpus == 0) {
        out.reason = "no_delta_cpus";
        return out;
    }

    // ---- 4. 转成差值友好的形态 ----
    std::vector<Impl::CpuTimes> cur;
    cur.reserve(local.hdr.nr_cpus);
    for (uint32_t i = 0; i < local.hdr.nr_cpus && i < WEAKNET_KMOD_MAX_CPUS; ++i) {
        const auto& c = local.cpu[i];
        Impl::CpuTimes t;
        t.cpu_id = c.cpu_id;
        t.online = (c.online != 0);
        t.valid = (c.valid != 0);
        // 与内核 ABI 头同口径：guest 已含在 user 内，不重复计入 busy
        t.busy = c.user + c.nice + c.system + c.irq + c.softirq + c.steal;
        t.total = t.busy + c.idle + c.iowait;
        cur.push_back(t);
    }

    uint32_t online = 0;
    for (const auto& t : cur) {
        if (t.online) ++online;
    }
    out.online_cpus = online;

    if (!impl_->have_prev) {
        impl_->prev = std::move(cur);
        impl_->have_prev = true;
        out.reason = "baseline_only";
        return out;
    }

    // ---- 5. 区间差值 → 加权比率 ----
    //
    // 三个必须跳过的条件：
    //   a) 本轮离线
    //   b) 上一轮离线（离线期间累计冻结，上线后基准不对）
    //   c) valid=0（内核已判定这一对快照不可相减，如跨热插拔）
    //
    // 条件 c) 是内核侧显式给出的契约，不能自己用 "cur > prev" 之类的
    // 启发式去猜：一个恰好满足 cur > prev 的错误组合会算出荒唐值。
    uint64_t sum_busy = 0, sum_total = 0;
    uint32_t used = 0;

    for (const auto& c : cur) {
        const Impl::CpuTimes* p = nullptr;
        for (const auto& pc : impl_->prev) {
            if (pc.cpu_id == c.cpu_id) {
                p = &pc;
                break;
            }
        }
        if (!p || !c.online || !p->online || !c.valid || !p->valid) continue;
        if (c.total < p->total || c.busy < p->busy) continue;  // 防御性：累计值不应倒退

        sum_busy += (c.busy - p->busy);
        sum_total += (c.total - p->total);
        ++used;
    }

    impl_->prev = std::move(cur);

    if (used == 0 || sum_total == 0) {
        out.reason = "no_delta_cpus";
        return out;
    }

    out.available = true;
    out.sampled_cpus = used;
    out.busy_ratio = static_cast<double>(sum_busy) / static_cast<double>(sum_total);
    // 数值防御：差值序列理论上不会越界，但除以极小 sum_total 时仍可能得到 >1
    if (out.busy_ratio > 1.0) out.busy_ratio = 1.0;
    if (out.busy_ratio < 0.0) out.busy_ratio = 0.0;
    return out;
}

// ---------------------------------------------------------------------------
// 软中断构成比采样器
// ---------------------------------------------------------------------------

struct HostSoftirqShareSampler::Impl {
    /// 单 CPU 的十类软中断累计次数。
    ///
    /// 注意这里**没有** valid 字段：ABI 里 wn_softirq_stat 就没有（软中断
    /// 计数不会被热插拔清零）。防倒退改成用户态自查，见 sample() 第 5 步。
    struct CpuCounts {
        uint32_t cpu_id{0};
        bool online{false};
        uint64_t cnt[WEAKNET_KMOD_NR_SOFTIRQ]{};
    };

    std::vector<CpuCounts> prev;
    bool have_prev{false};

    int fd{-1};
    void* map{MAP_FAILED};
    size_t map_size{0};
};

HostSoftirqShareSampler::HostSoftirqShareSampler(std::string dev_path)
    : dev_path_(std::move(dev_path)), impl_(std::make_unique<Impl>()) {}

HostSoftirqShareSampler::~HostSoftirqShareSampler() {
    if (impl_ && impl_->map != MAP_FAILED) {
        munmap(impl_->map, impl_->map_size);
    }
    if (impl_ && impl_->fd >= 0) {
        close(impl_->fd);
    }
}

HostSoftirqShare HostSoftirqShareSampler::sample() {
    HostSoftirqShare out;

    // ---- 1. 打开设备 + 映射（懒加载，每轮重试，支持模块后插）----
    if (impl_->fd < 0) {
        impl_->fd = ::open(dev_path_.c_str(), O_RDONLY | O_CLOEXEC);
        if (impl_->fd < 0) {
            const int open_errno = errno;
            out.reason = openErrnoToReason(open_errno);
            out.detail = std::string("open: ") + std::strerror(open_errno);
            return out;
        }
        // 必须用 softirq 自己的类型算长度。用 wn_cpu_snapshot 会得到更大的
        // 值，超过内核 g_map_size 后 mmap 直接 EINVAL（历史真实 bug）。
        impl_->map_size = sizeof(struct wn_softirq_snapshot);
        impl_->map = mmap(nullptr, impl_->map_size, PROT_READ, MAP_SHARED, impl_->fd, 0);
        if (impl_->map == MAP_FAILED) {
            out.reason = "mmap_failed";
            out.detail = std::string("mmap: ") + std::strerror(errno);
            close(impl_->fd);
            impl_->fd = -1;
            return out;
        }
    }

    // ---- 2. seqlock 读者：校验 → 拷贝 → 再校验 ----
    auto* snap = static_cast<struct wn_softirq_snapshot*>(impl_->map);
    struct wn_softirq_snapshot local;

    uint32_t s0 = 0, s1 = 0;
    bool ok = false;
    for (int tries = 0; tries < kMaxSeqRetry; ++tries) {
        s0 = loadSeq(&snap->hdr.seq);
        if (s0 & 1u) continue;

        std::memcpy(&local, snap, sizeof(local));

        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        s1 = loadSeq(&snap->hdr.seq);

        if (s0 == s1) {
            ok = true;
            break;
        }
    }
    if (!ok) {
        out.reason = "read_failed";
        return out;
    }

    // ---- 3. ABI 版本校验 ----
    if (local.hdr.abi_version != WEAKNET_KMOD_ABI_VERSION) {
        out.reason = "abi_mismatch";
        return out;
    }

    if (local.hdr.nr_cpus == 0) {
        out.reason = "no_delta_cpus";
        return out;
    }

    // ---- 4. 转成差值友好的形态 ----
    std::vector<Impl::CpuCounts> cur;
    cur.reserve(local.hdr.nr_cpus);
    for (uint32_t i = 0; i < local.hdr.nr_cpus && i < WEAKNET_KMOD_MAX_CPUS; ++i) {
        const auto& c = local.cpu[i];
        Impl::CpuCounts t;
        t.cpu_id = c.cpu_id;
        t.online = (c.online != 0);
        for (uint32_t k = 0; k < WEAKNET_KMOD_NR_SOFTIRQ; ++k) {
            t.cnt[k] = c.cnt[k];
        }
        cur.push_back(t);
    }

    uint32_t online = 0;
    for (const auto& t : cur) {
        if (t.online) ++online;
    }
    out.online_cpus = online;

    if (!impl_->have_prev) {
        impl_->prev = std::move(cur);
        impl_->have_prev = true;
        out.reason = "baseline_only";
        return out;
    }

    // ---- 5. 区间差值 → NET_RX 构成比 ----
    //
    // 与 CPU 采样器同款跳过条件（a 本轮离线 / b 上轮离线），再加一条
    // 本模块专有的防倒退检查（c）：
    //
    //   软中断计数是纯递增的，理论上不该倒退。但 ABI 没有 valid 标记可以
    //   依赖，而「理论上不该发生」正是最难发现的故障来源。任一类计数变小
    //   就跳过该 CPU——比起算出荒唐的负增量，少统计一个核的代价可以忽略。
    uint64_t net_rx = 0, total = 0;
    uint32_t used = 0;

    for (const auto& c : cur) {
        const Impl::CpuCounts* p = nullptr;
        for (const auto& pc : impl_->prev) {
            if (pc.cpu_id == c.cpu_id) {
                p = &pc;
                break;
            }
        }
        if (!p || !c.online || !p->online) continue;

        bool backwards = false;
        uint64_t cpu_total = 0;
        for (uint32_t k = 0; k < WEAKNET_KMOD_NR_SOFTIRQ; ++k) {
            if (c.cnt[k] < p->cnt[k]) {
                backwards = true;
                break;
            }
            cpu_total += (c.cnt[k] - p->cnt[k]);
        }
        if (backwards) continue;

        net_rx += (c.cnt[3] - p->cnt[3]);   // 索引 3 = NET_RX，见 ABI 头
        total += cpu_total;
        ++used;
    }

    impl_->prev = std::move(cur);

    out.sampled_cpus = used;
    out.net_rx_delta = net_rx;
    out.total_softirq_delta = total;

    if (used == 0) {
        out.reason = "no_delta_cpus";
        return out;
    }

    // 窗口内一次软中断都没触发（分母为 0）：构成比在数学上无定义。
    // 绝不回落成 0——0 会被读成「完全没有收包软中断」，是伪造的观测。
    if (total == 0) {
        out.reason = "no_softirq_activity";
        return out;
    }

    out.available = true;
    out.net_rx_share = static_cast<double>(net_rx) / static_cast<double>(total);
    if (out.net_rx_share > 1.0) out.net_rx_share = 1.0;
    if (out.net_rx_share < 0.0) out.net_rx_share = 0.0;
    return out;
}

namespace {

/// 双发一个 HOST 域指标到两处注册中心。
///
/// 抽成函数是为了让 CPU 与 softirq 两条发布路径**完全同构**——包括
/// 「不可用时发 UNAVAILABLE 绝不发 0」这条不能走样的纪律。
///
/// 【事实订正 2026-11-08，勿再沿用旧说法】
/// 此前这里的注释写的是「global() 那一份是 database_manager.cpp 唯一会读的，
/// 漏发会让指标永远进不了 SQLite」。**这句话是错的**，实测核对：
///   * database_manager.cpp:540 只调 latestGlobal()，读的是 GLOBAL 域，
///     且只读 TCP_RETRANS_LOSS_PCT / TCP_CONN_ACTIVE / TCP_CONN_ACCEPT_RATE；
///   * network_history 表没有任何 host 相关列；
///   * 全仓除本条外，`"host:"` 作用域只有 server.cpp 的读取方，无其它读者。
/// 也就是说 global() 里这份 HOST 域副本**当前没有任何消费者**，不会进 SQLite。
///
/// 仍然保留双发，只为与既有 CPU_BUSY_RATIO 路径保持同构（那条路径自
/// e5a979e 起就是这么写的），避免两条指标出现"一条发两处、一条发一处"
/// 的不一致；代价可忽略。**若要让 host 指标真正落库**，需要新增
/// network_history 列 + insertSnapshot 读取逻辑，那是另一件事，不要
/// 指望这里已经做好了。
void publishHostMetric(ServerContext* ctx, weaknet::MetricId id, bool available, double value) {
    if (!ctx->metrics_registry) return;
    const auto sample = available ? weaknet::MetricSample::valid(value)
                                  : weaknet::MetricSample::unavailable();
    ctx->metrics_registry->publishScope(weaknet::MetricScope::HOST, id, sample);
    weaknet::MetricsRegistry::global().publishScope(weaknet::MetricScope::HOST, id, sample);
}

}  // namespace

void start_host_pressure_monitor_thread(ServerContext* ctx, std::thread* worker) {
    // 可 join 句柄，由 MonitorManager 在停止时 join，避免 detached 线程在
    // ctx 析构后野访问（与 rtt_monitor 等既有监控器同一纪律）。
    *worker = std::thread([ctx] {
        LOG_INFO(LogModule::HOST_PRESSURE, "host pressure monitor thread started");

        HostCpuPressureSampler cpu_sampler;
        HostSoftirqShareSampler softirq_sampler;

        while (ctx->running.load() && !ctx->host_pressure_stop.load()) {
            try {
                const auto interval = readIntervalMs(ctx);

                // ---- 采样 1：CPU 忙碌比（时间维度）----
                //
                // 两个采样**顺序执行且互不短路**：即使 cpu_stat 不可用，
                // softirq 也照常采。两者是独立证据，任一个缺失都不该
                // 让另一个跟着哑掉。
                const HostCpuPressure p = cpu_sampler.sample();
                publishHostMetric(ctx, weaknet::MetricId::CPU_BUSY_RATIO,
                                 p.available, p.busy_ratio);

                if (p.available) {
                    LOG_INFO(LogModule::HOST_PRESSURE,
                             "cpu_busy_ratio=" << p.busy_ratio
                             << " (" << p.sampled_cpus << "/" << p.online_cpus
                             << " cpus sampled)");
                } else if (p.reason != "baseline_only") {
                    // 不可用时发布 UNAVAILABLE，**不发布 0**：
                    // 0 会被下游解读成"CPU 很闲"，从而掩盖真实故障。
                    // baseline_only 是启动后的正常状态，不值得刷警告。
                    //
                    // detail 必须打出来：reason 只说"打不开"，
                    // detail 才说"是 EPERM 还是 ENOENT"，而这两者
                    // 的处置完全不同（改 capability vs 装模块）。
                    LOG_WARNING(LogModule::HOST_PRESSURE,
                                "cpu pressure unavailable: " << p.reason
                                << (p.detail.empty() ? "" : " (" + p.detail + ")"));
                }

                // ---- 采样 2：软中断构成比（类型维度）----
                const HostSoftirqShare q = softirq_sampler.sample();
                publishHostMetric(ctx, weaknet::MetricId::SOFTIRQ_NET_RX_SHARE,
                                 q.available, q.net_rx_share);

                if (q.available) {
                    // 把分子分母一起打出来：只有构成比时，一个"38%"看不出
                    // 是几万次还是几亿次里算出来的，排障时没法判断量级。
                    LOG_INFO(LogModule::HOST_PRESSURE,
                             "softirq_net_rx_share=" << q.net_rx_share
                             << " (net_rx=" << q.net_rx_delta
                             << "/total=" << q.total_softirq_delta
                             << ", " << q.sampled_cpus << "/" << q.online_cpus
                             << " cpus sampled)");
                } else if (q.reason != "baseline_only") {
                    LOG_WARNING(LogModule::HOST_PRESSURE,
                                "softirq share unavailable: " << q.reason
                                << (q.detail.empty() ? "" : " (" + q.detail + ")"));
                }

                // 100 ms 分片等待：整段睡眠会让停止请求最长等一个完整周期
                const int slices = static_cast<int>(interval.count() / 100);
                for (int i = 0; i < slices && ctx->running.load() &&
                                !ctx->host_pressure_stop.load();
                     ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            } catch (const std::exception& e) {
                LOG_ERROR(LogModule::HOST_PRESSURE,
                          "host pressure monitor exception: " << e.what());
            } catch (...) {
                LOG_ERROR(LogModule::HOST_PRESSURE,
                          "host pressure monitor unknown exception");
            }
        }

        LOG_INFO(LogModule::HOST_PRESSURE, "host pressure monitor thread exiting");
    });
}

}  // namespace weaknet_dbus
