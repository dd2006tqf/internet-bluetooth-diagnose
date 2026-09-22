/**
 * @file rtt_monitor.hpp
 * @brief RTT（往返时间）+ 抖动（Jitter）监控线程启动接口
 *
 * 通过周期性 ICMP Echo 探测目标主机，测量网络往返时延，
 * 并在同一次采样上滑动窗口计算 RTT 标准差（即 Jitter，单位 ms）。
 * 结果写入 ServerContext::NetInfo::rtt_ms_ / jitter_ms_（哨兵值 -1 表示无效）。
 *
 * @note 内部实现使用原始 socket 发送 ICMP；需要 CAP_NET_RAW 或 root 权限，
 *       权限不足时线程静默跳过采样，保持 -1 哨兵值。
 */

#pragma once

#include <thread>

#include <string>

namespace weaknet_dbus {

class ServerContext;

/**
 * @brief 创建并启动 RTT + Jitter 监控线程
 *
 * 线程循环：对所有接口 ping host → 更新 RTT → 推入滑动窗口计算 Jitter →
 * 事件广播。原独立 jitter_monitor 已合并到本线程，消除重复 ICMP 探测。
 *
 * @param ctx         ServerContext 生命周期句柄；线程内通过它访问 WeakNetMgr
 * @param host        目标主机（IP 或域名，如 "1.1.1.1"、"8.8.8.8"）
 * @param intervalMs  采样间隔，默认 2000ms（2 秒一次）
 * @param timeoutMs   单次 ping 超时，默认 800ms（超过视为丢包）
 * @param windowSize  RTT 滑动窗口大小（样本数），<=0 表示不启用 jitter 统计
 */
void start_rtt_monitor_thread(ServerContext* ctx, std::thread* worker,
                              const std::string& host,
                              int intervalMs = 2000, int timeoutMs = 800,
                              int windowSize = 30);

}  // namespace weaknet_dbus
