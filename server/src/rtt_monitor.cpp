/**
 * @file rtt_monitor.cpp
 * @brief RTT（往返时延）+ 抖动（Jitter）周期监控线程实现
 *
 * 监控指标：
 *   - RTT（Round-Trip Time）：从发送 ICMP Echo 请求到收到 Echo Reply 的时间，单位 ms
 *   - Jitter（抖动值）：滑动窗口内 RTT 样本的总体标准差（ms），反映时延稳定性
 *   - 抖动等级：good(≤20ms)/degraded(≤50ms)/poor(>50ms)，参考 VoIP 质量标准
 *   - 网络质量等级：基于 RTT 和丢包率综合评估（WeakNetMgr::NetInfo::quality）
 *
 * 数据源：
 *   - 外部库：NetPing 类封装的 ICMP ping 实现（基于原始套接字 raw socket）
 *
 * 线程模型：
 *   - 单一 std::thread（通过 ServerContext 生命周期管理）
 *   - 与 RSSI/TCP Loss 等监控线程并行运行，通过 WeakNetMgr 的细粒度更新接口避免锁争用
 *   - 线程安全：ping 在**锁外**完成，再经 WeakNetMgr::updateRttAndStateForIfaceSafe
 *     与 updateJitterSafe 两个细粒度更新方法持锁写回；每轮对**所有接口**执行 ping，
 *     同时刷新 RTT 与 Jitter。
 *
 * 设计说明（合并自原 jitter_monitor）：
 *   原 jitter_monitor 是独立线程，对全部接口再次 ping 以维护滑动窗口；
 *   合并后本线程在每次 ping 拿到 RTT 后，直接把样本推入 per-iface 滑动窗口并
 *   计算标准差 —— 省掉一个线程、一份 NetPing 实例，以及重复的 ICMP 探测流量。
 */

#include <thread>
#include <chrono>
#include <cstdio>
#include <deque>
#include <map>
#include <cmath>
#include <string>

#include "server.hpp"
#include "dbus_service.hpp"
#include "weak_netmgr.hpp"
#include "rtt_monitor.hpp"
#include "net_ping.h"
#include "logger.hpp"

using namespace std::chrono_literals;

namespace weaknet_dbus {

namespace {

/**
 * @brief 根据抖动值（ms）判定抖动等级
 * @param jitterMs 抖动值，单位毫秒；负值表示无有效样本
 * @return 抖动等级字符串："good" / "degraded" / "poor" / "unknown"
 */
static std::string classifyJitterLevel(double jitterMs) {
    if (jitterMs < 0) return "unknown";
    if (jitterMs <= 20.0) return "good";
    if (jitterMs <= 50.0) return "degraded";
    return "poor";
}

/**
 * @brief 计算滑动窗口内 RTT 样本的抖动值（总体标准差）
 *
 * 仅统计有效样本（rtt >= 0），忽略超时返回的负值样本。
 * 样本数 < 2 时不足以计算标准差，返回 -1.0。
 *
 * 计算公式：σ = √(Σ(xᵢ - μ)² / N)
 *   其中 μ = Σxᵢ / N，N 为有效样本数
 *
 * @param samples RTT 样本滑动窗口（ms）
 * @return 抖动值（标准差，ms）；-1.0 表示样本不足
 */
static double calculateJitter(const std::deque<int>& samples) {
    int validCount = 0;
    double sum = 0.0;
    for (int s : samples) {
        if (s >= 0) {
            sum += s;
            ++validCount;
        }
    }
    if (validCount < 2) return -1.0;  // 至少需要 2 个样本才能计算标准差

    double mean = sum / validCount;
    double sqSum = 0.0;
    for (int s : samples) {
        if (s >= 0) {
            double diff = s - mean;
            sqSum += diff * diff;
        }
    }
    return std::sqrt(sqSum / validCount);
}

}  // namespace

/**
 * @brief 启动 RTT + Jitter 周期监控线程
 *
 * 线程以 intervalMs 为周期，对**所有网络接口**通过 ICMP ping 采集 RTT 样本，
 * 同步执行两项更新：
 *   1. 将 RTT 结果写入 WeakNetMgr 的 NetInfo::rttMs（含质量等级跃迁与 MetricsRegistry）
 *   2. 把样本推入 per-iface 滑动窗口（大小 windowSize），计算标准差与抖动等级，
 *      调用 WeakNetMgr::updateJitterSafe 写入 NetInfo::jitterMs / jitterLevel。
 *
 * @param ctx         ServerContext 指针，持有弱网管理器和 D-Bus 服务实例
 * @param host        探测目标主机（IP 或域名）
 * @param intervalMs  采样周期（毫秒）
 * @param timeoutMs   单次 ping 超时时间（毫秒）
 * @param windowSize  RTT 样本滑动窗口大小（样本数），用于计算标准差；<=0 表示不启用 jitter
 */
void start_rtt_monitor_thread(ServerContext* ctx, std::thread* worker,
                              const std::string& host, int intervalMs,
                              int timeoutMs, int windowSize) {
    // 加入可 join 句柄，由主线程退出路径 join，避免 detached 线程在 ctx 析构后野访问。
    // 循环内每轮从线程安全配置现读 target/interval/timeout/window_size，
    // 支持 D-Bus SetMonitorParam 实时调参。start 参数仅作 cfg 为空时的兜底。
    *worker = std::thread([ctx, host, intervalMs, timeoutMs, windowSize]{
        LOG_INFO(LogModule::RTT, "RTT+Jitter monitor thread started");
        auto pinger = NetPing::getInstance();

        // 每个接口维护独立的 RTT 样本窗口（key = 接口名，value = 样本 deque）
        std::map<std::string, std::deque<int>> sampleWindows;

        int loop_count = 0;
        while ((ctx->running.load() && !ctx->rtt_stop.load())) {
            loop_count++;
            try {
                // 每轮现读配置（D-Bus 调参立即生效）
                std::string eff_host = ctx->cfg.rtt.target.get();
                int eff_interval = ctx->cfg.rtt.interval_ms.load();
                int eff_timeout = ctx->cfg.rtt.timeout_ms.load();
                int eff_window  = ctx->cfg.rtt.window_size.load();
                if (eff_host.empty()) eff_host = host;
                if (eff_interval <= 0) eff_interval = intervalMs;
                if (eff_timeout <= 0) eff_timeout = timeoutMs;
                if (eff_window <= 0) eff_window = windowSize;

                // 遍历所有接口：ping 一次 → 更新 RTT + 推入窗口算 jitter。
                // 仅当 usingNow 接口发生 RTT 变化时才发信号（与原 RTT 行为一致），
                // jitter 变化通过 NetInfo 的字段对比由 weak_mgr 判断。
                bool rtt_changed = false;
                bool jitter_changed = false;
                {
                    auto current_interfaces = ctx->weak_mgr->getCurrentInterfaces();
                    for (const auto& net : current_interfaces) {
                        const std::string& ifname = net.ifName();

                        // ---- 单次 ICMP ping：同一次采样同时喂 RTT 和 Jitter ----
                        int rtt = pinger->ping(eff_host, ifname, eff_timeout);

                        // ---- 1) RTT 更新（复用既有细粒度接口，内部带锁）----
                        bool changed = ctx->weak_mgr->updateRttAndStateForIfaceSafe(ifname, rtt);
                        if (changed) rtt_changed = true;

                        // ---- 2) Jitter：推样本 → 算标准差 → 更新 ----
                        if (eff_window > 0) {
                            auto& window = sampleWindows[ifname];
                            window.push_back(rtt);
                            while (static_cast<int>(window.size()) > eff_window) {
                                window.pop_front();
                            }
                            double jitter = calculateJitter(window);
                            std::string level = classifyJitterLevel(jitter);
                            if (ctx->weak_mgr->updateJitterSafe(ifname, jitter, level)) {
                                jitter_changed = true;
                            }
                        }

                        // 输出 RTT/Jitter 监控信息（仅 usingNow 且变化时输出，减少日志量）
                        if (changed && net.usingNow()) {
                            LOG_INFO(LogModule::RTT, "RTT_MONITOR: " << ifname
                                << " | RTT: " << rtt << "ms"
                                << " | Quality: " << static_cast<int>(net.quality())
                                << " | Target: " << eff_host);
                        }
                    }
                }

                if ((rtt_changed || jitter_changed) && ctx->service) {
                    ctx->service->emitChanged("RTT/Jitter updated", /*counter*/0);
                }

                for (int i = 0; i < (eff_interval / 100) && (ctx->running.load() && !ctx->rtt_stop.load()); ++i)
                    std::this_thread::sleep_for(100ms);

            } catch (const std::exception& e) {
                LOG_ERROR(LogModule::RTT, "RTT/Jitter monitor thread exception: " << e.what());
            } catch (...) {
                LOG_ERROR(LogModule::RTT, "RTT/Jitter monitor thread unknown exception");
            }
        }
        LOG_INFO(LogModule::RTT, "RTT+Jitter monitor thread exiting");
    });
}

}  // namespace weaknet_dbus
