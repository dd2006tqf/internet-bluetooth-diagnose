/**
 * @file host_pressure_monitor.cpp
 * @brief 采集端自身 CPU 压力监控器实现
 *
 * 数据路径：
 *   kmod/weaknet_cpu_stat.c（内核 hrtimer，4 ms）
 *     → /dev/weaknet_cpu_stat（只读 mmap，seqlock 保护）
 *     → HostCpuPressureSampler::sample()（区间差值 → 加权比率）
 *     → MetricsRegistry::publishScope(MetricScope::HOST, CPU_BUSY_RATIO)
 *     → quality 线程 / 诊断归因读取
 *
 * 为什么这里直接 include kmod/weaknet_kmod_abi.h：
 *   那个头文件的存在意义就是"内核模块与用户态共享同一份结构定义"
 *   （见其文件头声明）。如果在服务端另抄一份，一旦内核侧改动结构体，
 *   服务端会在毫无征兆的情况下读到错位数据——而这个驱动 30 分钟长稳测试
 *   专门验过 seqlock 不撕裂，恰恰说明"读到的是稳定但错误的字段"这种
 *   故障最难发现。共享定义让编译器直接报错。
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

void start_host_pressure_monitor_thread(ServerContext* ctx, std::thread* worker) {
    // 可 join 句柄，由 MonitorManager 在停止时 join，避免 detached 线程在
    // ctx 析构后野访问（与 rtt_monitor 等既有监控器同一纪律）。
    *worker = std::thread([ctx] {
        LOG_INFO(LogModule::HOST_PRESSURE, "CPU pressure monitor thread started");

        HostCpuPressureSampler sampler;

        while (ctx->running.load() && !ctx->host_pressure_stop.load()) {
            try {
                const auto interval = readIntervalMs(ctx);

                const HostCpuPressure p = sampler.sample();

                if (ctx->metrics_registry) {
                    const auto sample = p.available
                        ? weaknet::MetricSample::valid(p.busy_ratio)
                        : weaknet::MetricSample::unavailable();
                    // 双发：与 server.cpp 的 TCP 重传/连接指标同一纪律。
                    // ctx->metrics_registry 服务评估侧读取（quality 线程归因），
                    // MetricsRegistry::global() 服务持久化侧读取
                    // （database_manager.cpp 的 insertSnapshot 只查 global）。
                    // 只发前者会让 CPU 压力永远进不了 SQLite——"看起来接好了、
                    // 实际永远 NULL"的典型陷阱。
                    ctx->metrics_registry->publishScope(
                        weaknet::MetricScope::HOST,
                        weaknet::MetricId::CPU_BUSY_RATIO,
                        sample);
                    weaknet::MetricsRegistry::global().publishScope(
                        weaknet::MetricScope::HOST,
                        weaknet::MetricId::CPU_BUSY_RATIO,
                        sample);

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
                          "CPU pressure monitor exception: " << e.what());
            } catch (...) {
                LOG_ERROR(LogModule::HOST_PRESSURE,
                          "CPU pressure monitor unknown exception");
            }
        }

        LOG_INFO(LogModule::HOST_PRESSURE, "CPU pressure monitor thread exiting");
    });
}

}  // namespace weaknet_dbus
