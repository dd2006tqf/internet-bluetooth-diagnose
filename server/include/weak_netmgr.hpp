/**
 * @file weak_netmgr.hpp
 * @brief 弱网管理器：聚合所有监控模块的数据更新入口
 *
 * WeakNetMgr 是数据更新的统一门面。13 个监控线程通过 updateXxxSafe() 系列方法
 * 更新内部维护的 iface_list_（以接口名为键的 NetInfo 列表）。DbusService 通过
 * getCurrentInterfaces() 读取最新快照作为 D-Bus 方法返回值。
 *
 * 设计模式：Facade（门面）。屏蔽各底层监控器（Ping / RSSI / eBPF / netlink）
 *   的差异，对外暴露统一的 NetInfo 读写接口。
 *
 * 线程安全：所有公开方法内部均通过 iface_mutex_ 保护共享状态。
 *   调用者只需调用 Safe 系列方法，无需额外加锁。
 */

#pragma once

#include <vector>
#include <string>
#include <memory>
#include <mutex>

#include "net_info.hpp"
#include "traffic_analyzer.hpp"

namespace weaknet_dbus {

/**
 * @brief 弱网管理器
 *
 * 核心数据：current_interfaces_（当前具备上网能力的接口列表）
 * 核心能力：collectCurrentInterfaces() 从底层查询接口名 → 后续各线程调用 updateXxxSafe() 填充指标
 */
class WeakNetMgr {
private:
    std::shared_ptr<TrafficAnalyzer> traffic_analyzer_;  ///< 流量分析器（可选，启动时可配置）
    mutable std::mutex iface_mutex_;                      ///< 保护接口列表的互斥锁
    std::vector<NetInfo> current_interfaces_;             ///< 当前接口列表

public:
    WeakNetMgr() : iface_mutex_(), current_interfaces_() {}

    // ==================== 接口列表采集 ====================

    /**
     * @brief 从底层 netlink 查询当前具备上网能力的接口
     *
     * 内部通过 net_iface.cpp 模块获取接口名，填充基本字段（名称、类型、状态）。
     * RTT/RSSI/丢包等深度指标由后续 updateXxxSafe 填充。
     *
     * @return 新构建的 NetInfo 列表（与 current_interfaces_ 独立，调用方决定是否替换）
     */
    std::vector<NetInfo> collectCurrentInterfaces();

    /**
     * @brief 在给定列表中按接口名查找条目
     * @param list    待搜索的接口列表
     * @param ifname  目标接口名
     * @param out     输出找到的 NetInfo（仅当返回 true 时有效）
     * @return true 找到；false 未找到
     */
    bool findByName(const std::vector<NetInfo>& list, const std::string& ifname, NetInfo* out) const;

    /**
     * @brief 静态工具：将 NetInfo 列表转为接口名数组
     * @param list 接口列表
     * @return 接口名字符串数组（用于 D-Bus ListInterfaces 返回值）
     */
    static std::vector<std::string> namesOf(const std::vector<NetInfo>& list);

    // ==================== 指标更新（非线程安全版本，仅供持有锁的调用方内部使用）====================

    /**
     * @brief 更新 current_interfaces_ 中每个接口的 usingNow 标志
     *
     * 通过 using_iface.cpp 模块解析默认路由表，标记哪个接口当前真正在上网。
     * 这会影响 NetworkQualityAssessor 的评估对象（仅评估 usingNow=true 的接口）。
     *
     * @param list      要更新的列表（按引用修改 usingNow 标志）
     * @param printLog true 时在变化时打印日志（调试用）
     * @param outIfName 可选：输出当前上网网卡名
     * @param outFlags  可选：输出路由表中的标志位
     * @return true 检测到变化（上网网卡切换或标志位改变）
     */
    bool updateCurrentUsing(std::vector<NetInfo>& list,
                            bool printLog,
                            std::string* outIfName = nullptr,
                            uint32_t* outFlags = nullptr);

    /**
     * @brief 对当前上网网卡执行 ICMP Ping 并更新 RTT 和链路质量
     * @param list     接口列表（按引用更新每个条目的 rttMs/quality）
     * @param host     目标域名/IP（通常是公共 DNS 如 223.5.5.5）
     * @param timeoutMs 单次 Ping 超时（默认 800ms）
     * @return true 有任何条目发生变化
     */
    bool updateRttAndState(std::vector<NetInfo>& list, const std::string& host, int timeoutMs = 800);

    /**
     * @brief 对所有 WiFi 类型接口查询 RSSI 信号强度
     *
     * 通过 wpa_supplicant ctrl_interface（UNIX DGRAM socket）发送 SIGNAL_POLL 命令。
     * 以太网/蜂窝接口自动跳过。
     *
     * @param list   接口列表
     * @param ctrlDir wpa_supplicant 控制目录（留空自动从 /var/run/wpa_supplicant 探测）
     */
    bool updateWifiRssi(std::vector<NetInfo>& list, const std::string& ctrlDir = "");

    /**
     * @brief 更新指定接口的 TCP 丢包率
     *
     * 由 TcpLossMonitor（eBPF 或 netlink SOCK_DIAG）调用，将计算结果写回 NetInfo。
     *
     * @param list       接口列表（在其中查找 iface_name）
     * @param iface_name 目标接口名
     * @param loss_rate  丢包率（百分比 0-100）
     * @param loss_level 丢包率等级（good/degraded/poor/insufficient）
     * @return true 成功更新且值发生变化
     */
    bool updateTcpLossRate(std::vector<NetInfo>& list,
                          const std::string& iface_name,
                          double loss_rate,
                          const std::string& loss_level);

    /**
     * @brief 更新指定接口的网络抖动
     *
     * 由 JitterMonitor 计算 RTT 样本标准差后调用。
     */
    bool updateJitter(std::vector<NetInfo>& list,
                      const std::string& iface_name,
                      double jitter_ms,
                      const std::string& jitter_level);

    // ==================== 流量分析 ====================

    /// 启动流量分析器：在指定接口上启动 eBPF flow_rate 采样
    void startTrafficAnalysis(const std::string& interface, int interval_seconds = 10);
    void stopTrafficAnalysis();
    /// 将 TrafficAnalyzer 最新结果回写到 NetInfo（traffic_total_bps/pps/active_flows）
    bool updateTrafficAnalysis(std::vector<NetInfo>& list);
    std::shared_ptr<TrafficAnalyzer> getTrafficAnalyzer() const;

    // ==================== 线程安全版本（Safe）====================
    // 这些方法内部持有 iface_mutex_ 锁，监控线程直接调用即可

    std::vector<NetInfo> getCurrentInterfaces() const;              ///< 读取最新快照（拷贝返回）
    void updateInterfaces(const std::vector<NetInfo>& new_interfaces); ///< 替换整个列表

    bool updateRttAndStateSafe(const std::string& host, int timeoutMs = 800);
    bool updateWifiRssiSafe(const std::string& ctrlDir = "");
    bool updateTcpLossRateSafe(const std::string& iface_name, double loss_rate, const std::string& loss_level);
    bool updateJitterSafe(const std::string& iface_name, double jitter_ms, const std::string& jitter_level);
    bool updateTrafficAnalysisSafe();
    bool updateCurrentUsingSafe();
};

}  // namespace weaknet_dbus
