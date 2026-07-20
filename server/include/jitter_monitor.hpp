// jitter_monitor.hpp
// 启动网络抖动(Jitter)监控线程
// 通过周期性 ICMP ping 采集 RTT 样本，计算标准差作为抖动值，
// 评估网络延迟的稳定性（对 VoIP/视频会议等实时业务尤为重要）

#pragma once

#include <string>

namespace weaknet_dbus {

class ServerContext;

// 创建并启动网络抖动监控线程
// host: 目标主机（如 223.5.5.5）
// intervalMs: 采样间隔（毫秒），默认 2000ms 以快速积累样本
// timeoutMs: 单次 ping 超时（毫秒）
// windowSize: 滑动窗口大小（样本数），默认 30
void start_jitter_monitor_thread(ServerContext* ctx,
                                 const std::string& host,
                                 int intervalMs = 2000,
                                 int timeoutMs = 800,
                                 int windowSize = 30);

}  // namespace weaknet_dbus
