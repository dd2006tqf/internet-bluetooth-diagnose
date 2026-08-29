// server.hpp
// 声明启动 DBus 服务端的入口函数，供 main 调用

#pragma once

#include <vector>
#include <string>
#include <mutex>
#include <thread>
#include <atomic>
#include <memory>

// 前置声明，避免强依赖 dbus 头
struct DBusConnection;

namespace weaknet_dbus {

class DbusService;  // 前置声明
class WeakNetMgr;   // 前置声明
class NetInfo;      // 前置声明
class BtMonitor;    // 前置声明
class DnsMonitor;                // 前置声明
class WifiPacketLossMonitor;     // 前置声明
class HttpLatencyMonitor;        // 前置声明
class ProcessNetProfiler;        // 前置声明
class TcpRetransMonitor;         // 前置声明
class DatabaseManager;           // 前置声明

struct ServerContext {
    // DBus 连接（保持原始指针，DBus 库管理）
    ::DBusConnection* connection = nullptr;

    // 运行标志
    std::atomic<bool> running{true};

    // 网卡监控线程
    std::thread iface_thread;
    // 当前上网网卡监控线程
    std::thread using_thread;
    // RTT 监控线程
    std::thread rtt_thread;
    // Jitter 监控线程
    std::thread jitter_thread;
    // Wi-Fi RSSI 监控线程
    std::thread rssi_thread;
    // 蓝牙监控线程
    std::thread bt_thread;
    // TCP丢包率监控线程
    std::thread tcp_loss_thread;
    // 流量分析线程
    std::thread traffic_analysis_thread;
    // 网络质量监控线程
    std::thread network_quality_thread;

    // 共享的可上网网卡列表（NetInfo）
    std::mutex iface_mutex;
    std::vector<NetInfo> iface_list;

    // 服务对象（方法处理与信号发送），智能指针管理生命周期
    std::unique_ptr<DbusService> service;

    // 弱网管理器，智能指针管理生命周期
    std::unique_ptr<WeakNetMgr> weak_mgr;

    // 蓝牙监测器，智能指针管理生命周期
    std::unique_ptr<BtMonitor> bt_monitor;

    // eBPF 监控器，智能指针管理生命周期
    std::unique_ptr<DnsMonitor> dns_monitor;
    std::unique_ptr<WifiPacketLossMonitor> wifi_loss_monitor;
    std::unique_ptr<HttpLatencyMonitor> http_latency_monitor;
    std::unique_ptr<ProcessNetProfiler> process_net_profiler;
    std::unique_ptr<TcpRetransMonitor> tcp_retrans_monitor;

    // eBPF 监控器线程
    std::thread dns_monitor_thread;
    std::thread wifi_loss_monitor_thread;
    std::thread http_latency_monitor_thread;
    std::thread process_net_profiler_thread;
    std::thread tcp_retrans_monitor_thread;

    // 历史数据持久化，智能指针管理生命周期
    std::unique_ptr<DatabaseManager> db_mgr;
    std::thread history_thread;

    ~ServerContext();
};

// 初始化 DBus（线程支持、连接、请求服务名、注册对象路径与回调）
// 返回连接指针，失败返回 nullptr
::DBusConnection* init_dbus(ServerContext* ctx);

// 启动网卡监控线程（维护接口列表并发送 Changed 信号）
void start_iface_monitor_thread(ServerContext* ctx);

// 启动 eBPF 监控器线程
void start_dns_monitor_thread(ServerContext* ctx);
void start_wifi_loss_monitor_thread(ServerContext* ctx);
void start_http_latency_monitor_thread(ServerContext* ctx);
void start_process_net_profiler_thread(ServerContext* ctx);
void start_tcp_retrans_monitor_thread(ServerContext* ctx);

// 启动历史数据持久化线程（每 5 分钟写入一次）
void start_history_persistence_thread(ServerContext* ctx);

// 启动流量分析线程（内部函数）
// void start_traffic_analysis_thread(ServerContext* ctx);

// 启动服务：
// - 创建 DBus 连接、注册对象与方法
// - 启动网卡监控线程，实时维护具备上网能力的网卡列表
// - 主线程运行 Looper::current()->run() 阻塞，避免程序退出
// 该函数会阻塞，直到进程被外部信号终止
int start_server();

}  // namespace weaknet_dbus


