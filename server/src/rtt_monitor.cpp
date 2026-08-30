/**
 * @file rtt_monitor.cpp
 * @brief RTT（往返时延）周期监控线程实现
 *
 * 监控指标：
 *   - RTT（Round-Trip Time）：从发送 ICMP Echo 请求到收到 Echo Reply 的时间，单位 ms
 *   - 网络质量等级：基于 RTT 和丢包率综合评估（WeakNetMgr::NetInfo::quality）
 *
 * 数据源：
 *   - 外部库：NetPing 类封装的 ICMP ping 实现（基于原始套接字 raw socket）
 *
 * 线程模型：
 *   - 单一 detached 风格的 std::thread（通过 ServerContext::rtt_thread 持有可 join 句柄）
 *   - 与 RSSI/Jitter/TCP Loss 等监控线程并行运行，通过 WeakNetMgr 的细粒度更新接口避免锁争用
 *   - 线程安全：仅调用 WeakNetMgr::updateRttAndStateSafe 线程安全更新方法
 */

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

/**
 * @brief 启动 RTT 周期监控线程
 *
 * 线程以 intervalMs 为周期，对目标 host 发送 ICMP ping，
 * 将结果写入 WeakNetMgr 的 NetInfo 列表中对应接口的 rttMs 字段，
 * 并更新网络质量等级。当 RTT 发生变化时，通过 D-Bus 服务发射变化信号。
 *
 * @param ctx         ServerContext 指针，持有弱网管理器和 D-Bus 服务实例
 * @param host        探测目标主机（IP 或域名）
 * @param intervalMs  采样周期（毫秒）
 * @param timeoutMs   单次 ping 超时时间（毫秒）
 */
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
