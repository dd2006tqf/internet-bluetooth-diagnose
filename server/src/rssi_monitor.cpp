/**
 * @file rssi_monitor.cpp
 * @brief Wi-Fi RSSI（接收信号强度指示）周期监控线程实现
 *
 * 监控指标：
 *   - RSSI（Received Signal Strength Indicator）：Wi-Fi 接口的接收信号强度，单位 dBm
 *     典型范围：-30 dBm（信号极好）~ -90 dBm（信号极弱）
 *
 * 数据源：
 *   - 系统调用：通过 Linux nl80211 netlink 接口查询 Wi-Fi 驱动的 BSS 信息
 *     （由 WeakNetMgr::updateWifiRssiSafe 内部封装实现）
 *
 * 线程模型：
 *   - 独立 std::thread，与 RTT/Jitter/TCP Loss 等监控线程并行
 *   - 线程安全：通过 WeakNetMgr::updateWifiRssiSafe 细粒度更新 NetInfo 的 rssiDbm 字段
 *   - 固定周期 10s（内部以 100ms 为单位睡眠，支持 ctx->running 快速响应退出）
 */

#include <thread>
#include <chrono>

#include "server.hpp"
#include "weak_netmgr.hpp"
#include "dbus_service.hpp"
#include "rssi_monitor.hpp"
#include "logger.hpp"

using namespace std::chrono_literals;

namespace weaknet_dbus {

/**
 * @brief 启动 Wi-Fi RSSI 周期监控线程
 *
 * 线程以固定周期（约 10 秒）查询所有 Wi-Fi 接口的 RSSI 值，
 * 通过 WeakNetMgr::updateWifiRssiSafe 线程安全地更新到 NetInfo 中。
 * 当 RSSI 发生变化时，通过 D-Bus 服务发射变化信号。
 *
 * @param ctx     ServerContext 指针
 * @param ctrlDir nl80211 控制目录路径（通常为 "/sys/class/net"，
 *                用于定位 Wi-Fi 接口设备并发起 netlink 查询）
 */
void start_rssi_monitor_thread(ServerContext* ctx, const std::string& ctrlDir) {
    // 加入可 join 句柄，由主线程退出路径 join，避免 detached 线程在 ctx 析构后野访问
    ctx->rssi_thread = std::thread([ctx, ctrlDir]{
        LOG_INFO(LogModule::RSSI, "RSSI monitor thread started");
        int loop_count = 0;
        while (ctx->running.load()) {
            loop_count++;

            // 直接调用线程安全的RSSI更新方法（内部通过 nl80211 netlink 查询驱动）
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
            // 固定 10s 周期：以 100ms 为单位睡眠，保证 ctx->running 能快速响应退出
            for (int i = 0; i < 100 && ctx->running.load(); ++i)
                std::this_thread::sleep_for(100ms);
        }
    });
}

}  // namespace weaknet_dbus
