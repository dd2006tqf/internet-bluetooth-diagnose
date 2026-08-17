// rtt_monitor.cpp
// 基于 NetPing 的 RTT 周期检测，并更新 WeakNetMgr 中 NetInfo 的 rtt 与质量

#include <thread>
#include <chrono>
#include <cstdio>

#include "server.hpp"
#include "dbus_service.hpp"
#include "weak_netmgr.hpp"
#include "rtt_monitor.hpp"
#include "logger.hpp"

using namespace std::chrono_literals;

namespace weaknet_dbus {

void start_rtt_monitor_thread(ServerContext* ctx, const std::string& host, int intervalMs, int timeoutMs) {
    // 加入可 join 句柄，由主线程退出路径 join，避免 detached 线程在 ctx 析构后野访问
    ctx->rtt_thread = std::thread([ctx, host, intervalMs, timeoutMs]{
        LOG_INFO(LogModule::RTT, "RTT monitor thread started");
        int loop_count = 0;
        while (ctx->running.load()) {
            loop_count++;
            try {
                // 直接调用线程安全的RTT更新方法
                bool changed = ctx->weak_mgr->updateRttAndStateSafe(host, timeoutMs);

                // 获取当前接口列表用于日志输出
                auto current_interfaces = ctx->weak_mgr->getCurrentInterfaces();

                // 输出RTT监控信息（仅在变化时输出，减少日志量）
                if (changed) {
                    for (const auto& net : current_interfaces) {
                        if (net.usingNow()) {
                            LOG_INFO(LogModule::RTT, "RTT_MONITOR: " << net.ifName()
                                << " | RTT: " << net.rttMs() << "ms"
                                << " | Quality: " << static_cast<int>(net.quality())
                                << " | Target: " << host);
                        }
                    }
                }

                if (changed && ctx->service) {
                    ctx->service->emitChanged("RTT/Quality updated", /*counter*/0);
                }

                for (int i = 0; i < (intervalMs / 100) && ctx->running.load(); ++i)
                    std::this_thread::sleep_for(100ms);

            } catch (const std::exception& e) {
                LOG_ERROR(LogModule::RTT, "RTT monitor thread exception: " << e.what());
            } catch (...) {
                LOG_ERROR(LogModule::RTT, "RTT monitor thread unknown exception");
            }
        }
        LOG_INFO(LogModule::RTT, "RTT monitor thread exiting");
    });
}

}  // namespace weaknet_dbus
