// jitter_monitor.cpp
// 网络抖动监控线程实现
// 原理：周期性发送 ICMP ping，收集 RTT 样本至滑动窗口，
//       计算标准差作为抖动值(Jitter)，并据此判定抖动等级。
// 抖动等级参考 VoIP 质量标准：
//   - good     : <= 20ms  （适合实时音视频）
//   - degraded : <= 50ms  （可用但体验下降）
//   - poor     : >  50ms  （实时业务卡顿明显）
//
// 线程安全：通过 WeakNetMgr::updateJitterSafe 细粒度更新 NetInfo 的抖动字段，
//           避免整体回写覆盖其他监控线程（RTT/RSSI/丢包率/流量）写入的字段。

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

// 根据抖动值(ms)判定等级
static std::string classifyJitterLevel(double jitterMs) {
    if (jitterMs < 0) return "unknown";
    if (jitterMs <= 20.0) return "good";
    if (jitterMs <= 50.0) return "degraded";
    return "poor";
}

// 计算滑动窗口内 RTT 样本的标准差（总体标准差）
// 仅统计有效样本（rtt >= 0），样本数 < 2 时返回 -1
static double calculateJitter(const std::deque<int>& samples) {
    int validCount = 0;
    double sum = 0.0;
    for (int s : samples) {
        if (s >= 0) {
            sum += s;
            ++validCount;
        }
    }
    if (validCount < 2) return -1.0;

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

void start_jitter_monitor_thread(ServerContext* ctx,
                                 const std::string& host,
                                 int intervalMs,
                                 int timeoutMs,
                                 int windowSize) {
    ctx->jitter_thread = std::thread([ctx, host, intervalMs, timeoutMs, windowSize]{
        LOG_INFO(LogModule::NETWORK, "Jitter monitor thread started (host=" << host
                 << ", interval=" << intervalMs << "ms, window=" << windowSize << ")");
        auto pinger = NetPing::getInstance();

        // 每个接口维护独立的 RTT 样本窗口
        std::map<std::string, std::deque<int>> sampleWindows;

        int loopCount = 0;
        while (ctx->running.load()) {
            loopCount++;
            try {
                // 获取当前接口列表（线程安全副本），仅用于遍历接口名
                auto currentInterfaces = ctx->weak_mgr->getCurrentInterfaces();
                bool anyChanged = false;

                for (const auto& net : currentInterfaces) {
                    const std::string& ifname = net.ifName();

                    // 确保该接口存在样本窗口
                    auto& window = sampleWindows[ifname];

                    // 发送 ICMP ping 采集 RTT 样本
                    int rtt = pinger->ping(host, ifname, timeoutMs);

                    // 维护滑动窗口
                    window.push_back(rtt);
                    while (static_cast<int>(window.size()) > windowSize) {
                        window.pop_front();
                    }

                    // 计算抖动
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

            for (int i = 0; i < (intervalMs / 100) && ctx->running.load(); ++i)
                std::this_thread::sleep_for(100ms);
        }
        LOG_INFO(LogModule::NETWORK, "Jitter monitor thread exiting");
    });
}

}  // namespace weaknet_dbus
