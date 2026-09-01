/**
 * @file jitter_monitor.cpp
 * @brief 网络抖动（Jitter）周期监控线程实现
 *
 * 监控指标：
 *   - Jitter（抖动值）：滑动窗口内 RTT 样本的总体标准差（ms），反映时延稳定性
 *   - 抖动等级：good(≤20ms)/degraded(≤50ms)/poor(>50ms)，参考 VoIP 质量标准
 *
 * 数据源：
 *   - 外部库：NetPing 类封装的 ICMP ping（与 RTT 监控共享同一 ping 实现）
 *
 * 线程模型：
 *   - 独立 std::thread，与 RTT/RSSI/TCP Loss 等监控线程并行
 *   - 每个网络接口维护独立的 RTT 样本滑动窗口（std::deque<int>）
 *   - 线程安全：通过 WeakNetMgr::updateJitterSafe 细粒度更新 NetInfo 的 jitter 字段，
 *     避免整体回写覆盖其他监控线程（RTT/RSSI/丢包率/流量）写入的字段
 *
 * 抖动等级判定（参考 ITU-T G.114 / VoIP 质量标准）：
 *   - good     : jitter ≤ 20ms  （适合实时音视频）
 *   - degraded : jitter ≤ 50ms  （可用但体验下降）
 *   - poor     : jitter >  50ms  （实时业务卡顿明显）
 */

#include <thread>
#include <chrono>
#include <deque>
#include <map>
#include <cmath>
#include <string>

#include "server.hpp"
#include "dbus_service.hpp"
#include "weak_netmgr.hpp"
#include "net_ping.h"
#include "jitter_monitor.hpp"
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
            sum += s;       // 累计有效 RTT 值
            ++validCount;   // 统计有效样本数
        }
    }
    if (validCount < 2) return -1.0;  // 至少需要 2 个样本才能计算标准差

    double mean = sum / validCount;   // 计算有效样本均值 μ
    double sqSum = 0.0;
    for (int s : samples) {
        if (s >= 0) {
            double diff = s - mean;    // 计算每个样本与均值的偏差
            sqSum += diff * diff;      // 累计偏差平方和 Σ(xᵢ - μ)²
        }
    }
    return std::sqrt(sqSum / validCount);  // 总体标准差 σ = √(Σ(xᵢ - μ)² / N)
}

}  // namespace

/**
 * @brief 启动网络抖动周期监控线程
 *
 * 线程以 intervalMs 为周期，对每个网络接口通过 ICMP ping 采集 RTT 样本，
 * 维护独立的滑动窗口（窗口大小 windowSize），计算抖动值和抖动等级，
 * 并通过 WeakNetMgr::updateJitterSafe 线程安全地更新到 NetInfo 中。
 *
 * @param ctx         ServerContext 指针
 * @param host        探测目标主机
 * @param intervalMs  采样周期（毫秒）
 * @param timeoutMs   单次 ping 超时时间（毫秒）
 * @param windowSize  RTT 样本滑动窗口大小（样本数），用于计算标准差
 */
void start_jitter_monitor_thread(ServerContext* ctx,
                                 const std::string& host,
                                 int intervalMs,
                                 int timeoutMs,
                                 int windowSize) {
    ctx->jitter_thread = std::thread([ctx, host, intervalMs, timeoutMs, windowSize]{
        LOG_INFO(LogModule::NETWORK, "Jitter monitor thread started");
        auto pinger = NetPing::getInstance();

        // 每个接口维护独立的 RTT 样本窗口（key = 接口名，value = 样本 deque）
        std::map<std::string, std::deque<int>> sampleWindows;

        int loopCount = 0;
        while (ctx->running.load()) {
            loopCount++;
            // 每轮现读配置（D-Bus 调参立即生效）
            std::string eff_host = ctx->cfg.jitter.target.get();
            int eff_interval = ctx->cfg.jitter.interval_ms.load();
            int eff_timeout = ctx->cfg.jitter.timeout_ms.load();
            int eff_window = ctx->cfg.jitter.window_size.load();
            if (eff_host.empty()) eff_host = host;
            if (eff_interval <= 0) eff_interval = intervalMs;
            if (eff_timeout <= 0) eff_timeout = timeoutMs;
            if (eff_window <= 0) eff_window = windowSize;

            try {
                // 获取当前接口列表（线程安全副本），仅用于遍历接口名
                auto currentInterfaces = ctx->weak_mgr->getCurrentInterfaces();
                bool anyChanged = false;

                for (const auto& net : currentInterfaces) {
                    const std::string& ifname = net.ifName();

                    // 确保该接口存在样本窗口（第一次访问时自动创建空 deque）
                    auto& window = sampleWindows[ifname];

                    // 发送 ICMP ping 采集 RTT 样本（负值表示超时）
                    int rtt = pinger->ping(eff_host, ifname, eff_timeout);

                    // 维护滑动窗口：新样本入队，超过窗口大小则丢弃最旧样本
                    window.push_back(rtt);
                    while (static_cast<int>(window.size()) > eff_window) {
                        window.pop_front();
                    }

                    // 计算抖动（标准差）和抖动等级
                    double jitter = calculateJitter(window);
                    std::string level = classifyJitterLevel(jitter);

                    // 线程安全地更新 NetInfo 的抖动字段（细粒度，不覆盖其他字段）
                    bool changed = ctx->weak_mgr->updateJitterSafe(ifname, jitter, level);
                    if (changed) anyChanged = true;

                    LOG_INFO(LogModule::NETWORK, "JITTER_MONITOR: " << ifname
                             << " | RTT: " << rtt << "ms"
                             << " | Jitter: " << jitter << "ms"
                             << " | Level: " << level
                             << " | Samples: " << window.size()
                             << " | Using: " << (net.usingNow() ? "YES" : "NO"));
                }

                if (anyChanged && ctx->service) {
                    LOG_INFO(LogModule::NETWORK, "Jitter updated - emitting signal");
                    // 同步发射（内部有锁），不创建 detached 子线程
                    ctx->service->emitChanged("Jitter updated", /*counter*/0);
                }
            } catch (const std::exception& e) {
                LOG_ERROR(LogModule::NETWORK, "Jitter monitor thread exception: " << e.what());
            } catch (...) {
                LOG_ERROR(LogModule::NETWORK, "Jitter monitor thread unknown exception");
            }

            for (int i = 0; i < (eff_interval / 100) && ctx->running.load(); ++i)
                std::this_thread::sleep_for(100ms);
        }
        LOG_INFO(LogModule::NETWORK, "Jitter monitor thread exiting");
    });
}

}  // namespace weaknet_dbus
