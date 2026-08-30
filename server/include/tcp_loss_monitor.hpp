/**
 * @file tcp_loss_monitor.hpp
 * @brief TCP 丢包率监控线程启动接口
 *
 * 从 /proc/net/snmp 读取 TCP 累计计数（InSegs / OutSegs / RetransSegs），
 * 对两次采样做差分计算丢包率：lossRate = deltaRetransSegs / deltaOutSegs * 100%。
 *
 * 结果写入 ServerContext::NetInfo::tcp_loss_rate_（哨兵值 -1.0 表示无效）。
 *
 * @note 当 deltaOutSegs < 10 时，流量过低不足以计算有意义的丢包率，
 *       输出 INSUFFICIENT 等级（不更新丢包率字段或保留上一有效值）。
 *       本实现是 Phase 1 方案；Phase 2 的 TcpRetransMonitor（eBPF）更精准，
 *       可按连接粒度输出。
 */

#pragma once

#include "server.hpp"

namespace weaknet_dbus {

/**
 * @brief 启动 TCP 丢包率监控线程
 *
 * 监控当前上网网卡的 TCP 丢包率，并更新到 ServerContext 中的 NetInfo 列表。
 * 线程循环：采样 → 差分 → 计算 → WeakNetMgr 更新 → 事件广播。
 *
 * @param ctx  ServerContext 生命周期句柄；线程内通过它访问 WeakNetMgr
 */
void start_tcp_loss_monitor_thread(ServerContext* ctx);

} // namespace weaknet_dbus
