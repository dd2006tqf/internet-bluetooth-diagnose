/**
 * @file server.hpp
 * @brief WeakNet 服务端生命周期中枢与启动入口
 *
 * 本文件定义 ServerContext（全局生命周期结构体）以及服务启动相关的自由函数。
 * ServerContext 聚合了 D-Bus 连接、所有监控线程、eBPF 监控器实例和共享数据，
 * 确保所有资源在同一对象内集中管理，退出时按依赖逆序释放。
 *
 * 设计约束：
 *   - 所有捕获 ServerContext 的线程必须可 join（禁止 std::thread::detach）
 *   - eBPF 监控器由 unique_ptr 持有 ownership，线程仅通过裸指针访问
 *   - 共享数据 iface_list 受 iface_mutex 保护
 */

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

class DbusService;          // 前置声明：D-Bus 方法处理与信号发送
class WeakNetMgr;           // 前置声明：弱网管理器（接口列表 + 质量指标聚合）
class NetInfo;              // 前置声明：单个网卡的状态与指标模型
class BtMonitor;            // 前置声明：蓝牙监测器
class DnsMonitor;           // 前置声明：DNS 监控（eBPF）
class WifiPacketLossMonitor;// 前置声明：Wi-Fi 丢包归因（eBPF）
class HttpLatencyMonitor;   // 前置声明：HTTP 请求延迟（eBPF）
class ProcessNetProfiler;   // 前置声明：进程网络画像（eBPF）
class TcpRetransMonitor;    // 前置声明：TCP 重传监控（eBPF）
class DatabaseManager;      // 前置声明：SQLite 历史数据持久化

/**
 * @brief 服务端全局生命周期中枢
 *
 * 持有所有监控线程、eBPF 监控器、D-Bus 服务对象和共享数据。
 * 析构函数中按依赖逆序释放资源：先停止线程（join），再释放 eBPF 监控器和 DBusService。
 *
 * 线程安全约束：
 *   - running_: 原子变量，所有线程轮询检查退出信号
 *   - iface_list: 多线程并发读写，必须通过 iface_mutex_ 保护
 *   - service/weak_mgr/bt_monitor/dns_monitor 等 unique_ptr: 启动后只读访问，无需额外锁
 */
struct ServerContext {
    ::DBusConnection* connection = nullptr;   ///< D-Bus 会话总线连接（由 DBus 库管理生命周期，析构时需显式 disconnection）

    std::atomic<bool> running{true};          ///< 全局运行标志。设为 false 后所有监控线程在下一个周期退出

    // ---------- 传统监控线程（非 eBPF）----------
    std::thread iface_thread;                 ///< 网卡列表监控：netlink ROUTE 组播驱动，维护可用接口列表
    std::thread using_thread;                 ///< 当前上网网卡监控：解析默认路由，标记 usingNow
    std::thread rtt_thread;                   ///< RTT 延迟监控：ICMP Ping 到 223.5.5.5
    std::thread jitter_thread;                ///< 网络抖动监控：基于 RTT 样本的标准差
    std::thread rssi_thread;                  ///< Wi-Fi RSSI 监控：通过 wpa_supplicant ctrl_interface 获取信号强度
    std::thread bt_thread;                    ///< 蓝牙监控：BlueZ D-Bus 系统总线轮询
    std::thread tcp_loss_thread;              ///< TCP 丢包率监控：netlink SOCK_DIAG 或 eBPF tcp_retransmit
    std::thread traffic_analysis_thread;      ///< 流量分析：/proc/net/dev + eBPF flow_rate
    std::thread network_quality_thread;       ///< 综合网络质量评估：聚合 RTT + RSSI + 丢包 + 流量

    // ---------- 共享数据 ----------
    std::mutex iface_mutex;                   ///< 保护 iface_list 的并发访问（多写多读场景）
    std::vector<NetInfo> iface_list;          ///< 当前所有具备上网能力的网卡列表（共享状态）

    // ---------- 服务对象（unique_ptr 管理生命周期）----------
    std::unique_ptr<DbusService> service;                ///< D-Bus 服务：导出方法 + 发射信号
    std::unique_ptr<WeakNetMgr> weak_mgr;                ///< 弱网管理器：聚合所有监控器的数据更新
    std::unique_ptr<BtMonitor> bt_monitor;               ///< 蓝牙监测器实例

    // ---------- eBPF 监控器（unique_ptr 管理 ownership）----------
    std::unique_ptr<DnsMonitor> dns_monitor;             ///< DNS 解析延迟/超时监控
    std::unique_ptr<WifiPacketLossMonitor> wifi_loss_monitor; ///< Wi-Fi 收发丢包归因
    std::unique_ptr<HttpLatencyMonitor> http_latency_monitor; ///< HTTP 请求级 TTFB 延迟
    std::unique_ptr<ProcessNetProfiler> process_net_profiler; ///< 每进程带宽/重传统计
    std::unique_ptr<TcpRetransMonitor> tcp_retrans_monitor;   ///< TCP 连接级重传追踪

    // ---------- eBPF 监控器线程 ----------
    std::thread dns_monitor_thread;
    std::thread wifi_loss_monitor_thread;
    std::thread http_latency_monitor_thread;
    std::thread process_net_profiler_thread;
    std::thread tcp_retrans_monitor_thread;

    // ---------- 历史数据持久化 ----------
    std::unique_ptr<DatabaseManager> db_mgr;   ///< SQLite 管理器，持有数据库连接
    std::thread history_thread;                 ///< 每 5 分钟将 iface_list 快照写入 DB

    /**
     * @brief 析构：释放所有资源
     *
     * 执行顺序：
     *   1. running_ = false（通知所有监控线程退出）
     *   2. join 所有监控线程
     *   3. unique_ptr 析构 DbusService → WeakNetMgr → eBPF 监控器（释放 BPF link 和 map）
     *   4. 关闭 DBus 连接
     *
     * 注意：析构时 running_ 必须已设为 false，否则监控线程可能访问已释放的指针。
     */
    ~ServerContext();
};

/**
 * @brief 初始化 D-Bus 会话总线连接
 *
 * 执行步骤：
 *   1. dbus_threads_init_default() 启用线程安全
 *   2. dbus_bus_get(DBUS_BUS_SESSION) 获取会话总线连接
 *   3. dbus_bus_request_name() 注册 com.example.WeakNet 服务名
 *   4. 将连接存入 ctx->connection
 *
 * @param ctx 全局上下文，初始化后 connection 字段会被填充
 * @return DBus 连接指针，失败时返回 nullptr
 */
::DBusConnection* init_dbus(ServerContext* ctx);

/**
 * @brief 启动网卡列表监控线程
 *
 * 通过 netlink ROUTE 组播监听接口添加/删除事件，
 * 定期刷新 ctx->iface_list 并通过 D-Bus 发射 InterfaceChanged 信号。
 *
 * @param ctx 全局上下文（线程捕获裸指针，退出前必须 join）
 */
void start_iface_monitor_thread(ServerContext* ctx);

/**
 * @brief 启动 DNS eBPF 监控线程
 *
 * 加载 dns_monitor.bpf.o（挂载 kprobe/udp_sendmsg + kprobe/udp_recvmsg），
 * 每 10 秒从 BPF map 读取 DNS 查询统计并输出日志。
 *
 * @param ctx 全局上下文
 */
void start_dns_monitor_thread(ServerContext* ctx);

/**
 * @brief 启动 Wi-Fi 丢包归因 eBPF 监控线程
 *
 * 加载 wifi_packet_loss.bpf.o（挂载 tracepoint/netif_receive_skb 等），
 * 按 ifindex 区分收发方向丢包，识别是否 Wi-Fi 空口原因导致。
 *
 * @param ctx 全局上下文
 */
void start_wifi_loss_monitor_thread(ServerContext* ctx);

/**
 * @brief 启动 HTTP 延迟 eBPF 监控线程
 *
 * 加载 http_latency.bpf.o（挂载 tcp_sendmsg + tcp_recvmsg），
 * 按 TCP 连接聚合事务，输出 p50/p99 TTFB 延迟和慢请求 Top N。
 *
 * @param ctx 全局上下文
 */
void start_http_latency_monitor_thread(ServerContext* ctx);

/**
 * @brief 启动进程网络画像 eBPF 监控线程
 *
 * 加载 flow_rate.bpf.o（同 tcp_retransmit_skb 探针的多消费者之一），
 * 按 PID 聚合 TX/RX 字节和重传次数，输出 Top N 带宽进程。
 *
 * @param ctx 全局上下文
 */
void start_process_net_profiler_thread(ServerContext* ctx);

/**
 * @brief 启动 TCP 重传监控线程
 *
 * 加载独立的 tcp_retransmit.bpf.o（也挂在 tcp_retransmit_skb 上），
 * 按连接（src/dst IP+port）聚合丢包率，与 ProcessNetProfiler 是同探针多消费者架构。
 *
 * @param ctx 全局上下文
 */
void start_tcp_retrans_monitor_thread(ServerContext* ctx);

/**
 * @brief 启动历史数据持久化线程
 *
 * 每 5 分钟遍历 ctx->iface_list，将每个 NetInfo 快照写入 SQLite。
 * 数据库路径由 WEAKNET_DATA_DIR 环境变量决定，默认 /home/radxa/weaknet/data/history.db。
 *
 * @param ctx 全局上下文
 */
void start_history_persistence_thread(ServerContext* ctx);

/**
 * @brief 启动 WeakNet D-Bus 服务端主入口
 *
 * 完整启动流程：
 *   1. 创建 ServerContext（unique_ptr 智能持有）
 *   2. init_dbus() 建立会话总线连接、注册服务名 com.example.WeakNet
 *   3. 创建 DbusService / WeakNetMgr / eBPF 监控器实例
 *   4. 启动 13+ 个监控线程 + 历史持久化线程
 *   5. 主线程进入 Looper::run() 阻塞，处理 D-Bus 消息
 *
 * 该函数会阻塞，直到进程被外部信号（SIGINT/SIGTERM）终止或 ServerContext::running_ 被置 false。
 * 退出时 ServerContext 析构函数负责按逆序停止所有线程和释放资源。
 *
 * @return 0 表示正常退出，非零表示初始化失败
 */
int start_server();

}  // namespace weaknet_dbus
