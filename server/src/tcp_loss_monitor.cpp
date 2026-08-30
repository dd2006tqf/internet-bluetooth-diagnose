/**
 * @file tcp_loss_monitor.cpp
 * @brief TCP 丢包率周期监控线程实现
 *
 * 监控指标：
 *   - TCP 丢包率（%）：基于 TCP 发送段数与重传段数的比值
 *     计算公式：lossRate = retransSegs / totalSegs × 100%
 *   - 丢包等级：根据丢包率阈值分类（由 net_tcp.h 中的 TcpLossMonitor 定义）
 *
 * 数据源：
 *   - 系统调用：读取 /proc/net/snmp 中的 Tcp 段统计（SegmentsOut/RetransSegs），
 *     由 TcpLossMonitor::sampleForInterface 封装实现
 *   - 注意：此处基于 /proc 文本解析，与 eBPF 的 tcp_retransmit_monitor 形成互补
 *
 * 线程模型：
 *   - 独立 std::thread，与 RTT/RSSI/Jitter 等监控线程并行
 *   - 差分采样：保存前一轮的 TcpStats，通过前后两次采样的差值计算丢包率
 *   - 线程安全：通过 WeakNetMgr::updateTcpLossRateSafe 细粒度更新 NetInfo 的 tcpLossRate 字段
 *
 * 丢包率计算逻辑：
 *   1. 采样前次统计 prevStats 和当前统计 currStats
 *   2. 计算差分：sentDelta = curr.segs_out - prev.segs_out
 *               retransDelta = curr.retrans_segs - prev.retrans_segs
 *   3. 仅当 sentDelta ≥ 10 时才计算丢包率（避免低流量时百分比剧烈波动）
 *   4. lossRate = retransDelta / sentDelta × 100%
 */

#include "server.hpp"
#include "tcp_loss_monitor.hpp"
#include "net_tcp.h"
#include "weak_netmgr.hpp"
#include "dbus_service.hpp"
#include "logger.hpp"
#include <cstdio>
#include <chrono>

using namespace std::chrono_literals;

namespace weaknet_dbus {

/**
 * @brief TCP 丢包率监控线程函数
 *
 * 以 10s 为周期（内部以 100ms 为单位睡眠），对当前活动网卡进行 TCP 统计差分采样，
 * 计算丢包率并更新到 WeakNetMgr。当丢包等级变化时输出日志。
 *
 * @param ctx ServerContext 指针，持有弱网管理器和 D-Bus 服务实例
 */
void start_tcp_loss_monitor_thread(ServerContext* ctx) {
    ctx->tcp_loss_thread = std::thread([ctx](){
        LOG_INFO(LogModule::TCP_LOSS, "monitor thread started");

        auto tcpMonitor = TcpLossMonitor::getInstance();
        TcpStats prevStats, currStats;
        bool hasPrevStats = false;   // 是否已有前一次采样值（首轮跳过丢包率计算）

        int loop_count = 0;
        while (ctx->running.load()) {
            loop_count++;

            // 获取当前上网网卡信息
            auto current_interfaces = ctx->weak_mgr->getCurrentInterfaces();
            std::string currentIface;
            for (const auto& net : current_interfaces) {
                if (net.usingNow()) {
                    currentIface = net.ifName();
                    break;
                }
            }

            // 未找到活动网卡，等待 5s 后重试
            if (currentIface.empty()) {
                for (int i = 0; i < 50 && ctx->running.load(); ++i)
                    std::this_thread::sleep_for(100ms);
                continue;
            }

            // 采样当前 TCP 统计信息（从 /proc/net/snmp 读取）
            if (!tcpMonitor->sampleForInterface(currentIface, currStats)) {
                LOG_ERROR(LogModule::TCP_LOSS, "failed to sample TCP stats for interface: " << currentIface);
                for (int i = 0; i < 100 && ctx->running.load(); ++i)
                    std::this_thread::sleep_for(100ms);
                continue;
            }

            // 计算丢包率（仅当有前一次采样数据时）
            if (hasPrevStats) {
                TcpLossResult result = tcpMonitor->compute(prevStats, currStats);

                // 只有当发送的数据包超过阈值（≥10）时才计算，避免低流量时百分比剧烈波动
                if (result.sentDelta >= 10) {
                    // 仅在丢包等级变化时输出日志，减少日志量
                    static std::string lastLevel;
                    if (result.level != lastLevel) {
                        LOG_INFO(LogModule::TCP_LOSS, "TCP_LOSS_MONITOR: interface=" << currentIface
                            << " rate=" << result.ratePercent << "%"
                            << " level=" << result.level);
                        lastLevel = result.level;
                    }

                    // 更新到 weak_mgr 中保存的 NetInfo 列表（线程安全）
                    bool updated = ctx->weak_mgr->updateTcpLossRateSafe(currentIface, result.ratePercent, result.level);
                    if (updated && ctx->service) {
                        std::string msg = std::string("TCP loss rate updated for ") + currentIface +
                                         ": " + std::to_string(result.ratePercent) + "% (" + result.level + ")";
                        ctx->service->emitChanged(msg, /*counter*/0);
                    }
                }
            }

            // 保存当前统计作为下次的前次统计（为下一轮差分计算做准备）
            prevStats = currStats;
            hasPrevStats = true;

            for (int i = 0; i < 100 && ctx->running.load(); ++i)
                std::this_thread::sleep_for(100ms);
        }

        LOG_INFO(LogModule::TCP_LOSS, "monitor thread terminated");
    });
}

} // namespace weaknet_dbus
