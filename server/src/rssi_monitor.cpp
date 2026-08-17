// rssi_monitor.cpp
// 对 Wi-Fi 接口周期获取 RSSI 并更新 WeakNetMgr 内的 NetInfo

#include <thread>
#include <chrono>

#include "server.hpp"
#include "weak_netmgr.hpp"
#include "dbus_service.hpp"
#include "rssi_monitor.hpp"
#include "logger.hpp"

using namespace std::chrono_literals;

namespace weaknet_dbus {

void start_rssi_monitor_thread(ServerContext* ctx, const std::string& ctrlDir) {
    // 加入可 join 句柄，由主线程退出路径 join，避免 detached 线程在 ctx 析构后野访问
    ctx->rssi_thread = std::thread([ctx, ctrlDir]{
        LOG_INFO(LogModule::RSSI, "RSSI monitor thread started");
        int loop_count = 0;
        while (ctx->running.load()) {
            loop_count++;

            // 直接调用线程安全的RSSI更新方法
            bool changed = ctx->weak_mgr->updateWifiRssiSafe(ctrlDir);

            // 获取当前接口列表用于日志输出
            auto current_interfaces = ctx->weak_mgr->getCurrentInterfaces();

            // 输出RSSI监控信息（仅在变化时输出，减少日志量）
            if (changed) {
                for (const auto& net : current_interfaces) {
                    if (net.type() == NetType::WiFi && net.usingNow()) {
                        LOG_INFO(LogModule::RSSI, "RSSI_MONITOR: " << net.ifName()
                            << " | RSSI: " << net.rssiDbm() << "dBm");
                    }
                }
            }

            if (changed && ctx->service) {
                ctx->service->emitChanged("WiFi RSSI updated", /*counter*/0);
            }
            for (int i = 0; i < 100 && ctx->running.load(); ++i)
                std::this_thread::sleep_for(100ms);
        }
    });
}

}  // namespace weaknet_dbus
