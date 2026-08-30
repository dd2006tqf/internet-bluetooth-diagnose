/**
 * @file rssi_monitor.hpp
 * @brief Wi-Fi RSSI（接收信号强度）监控线程启动接口
 *
 * 通过 wpa_supplicant 的 UNIX DGRAM 控制接口发送 SIGNAL_POLL 命令，
 * 解析 BSS 参数中的 RSSI 值（dBm）。
 *
 * 结果写入 ServerContext::NetInfo::rssi_dbm_（哨兵值 -1000 表示无效）。
 *
 * @note 需要 wpa_supplicant 正在运行且 /var/run/wpa_supplicant 可访问。
 *       若系统使用 NetworkManager 管理 Wi-Fi，supplicant 控制路径可能不同，
 *       可通过 ctrlDir 参数指定自定义路径。
 */

#pragma once

#include <string>

namespace weaknet_dbus {

class ServerContext;

/**
 * @brief 创建并启动 Wi-Fi RSSI 监控线程
 *
 * 线程内部通过 WeakNetMgr 获取当前活跃网卡名，再连接对应 wpa_supplicant 控制通道。
 *
 * @param ctx       ServerContext 生命周期句柄
 * @param ctrlDir   wpa_supplicant 控制目录，默认 "/var/run/wpa_supplicant"；
 *                  传空字符串则使用默认值
 */
void start_rssi_monitor_thread(ServerContext* ctx, const std::string& ctrlDir = "");

}  // namespace weaknet_dbus
