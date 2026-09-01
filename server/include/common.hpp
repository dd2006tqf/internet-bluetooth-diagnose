/**
 * @file common.hpp
 * @brief D-Bus 服务常量与全局配置定义
 *
 * 集中管理服务标识（bus name / object path / interface）、
 * 方法名、信号名以及运行时可配置的路径常量。
 * 路径常量支持通过环境变量覆盖，便于交叉编译部署和测试。
 *
 * 线程安全：本文件所有静态/inline 常量在编译期初始化，
 * 首次访问时求值（函数体风格的 lambda），无需额外锁。
 */

#pragma once

#include <string>

namespace weaknet_dbus {

// ==================== D-Bus 基础标识 ====================
// 这三个值共同决定了 D-Bus 服务的寻址三元组：BusName.ObjectPath.Interface
// 客户端必须使用完全相同的值才能正确连接到服务端

static constexpr const char kBusName[]     = "com.example.WeakNet";   ///< 服务名（唯一，由 dbus_bus_request_name 注册）
static constexpr const char kObjectPath[]  = "/com/example/WeakNet"; ///< 对象路径（类似文件系统，必须以 / 开头）
static constexpr const char kInterface[]    = "com.example.WeakNet"; ///< 接口名（方法和信号的命名空间）

// ==================== D-Bus 方法名 ====================
// 每个方法对应 DbusService 中一个 handleXxx 成员函数实现
// 客户端通过 dbus_message_new_method_call(..., kMethodXxx) 发起调用

static constexpr const char kMethodGet[]                    = "Get";                ///< 示例方法：返回服务标识字符串（调试用）
static constexpr const char kMethodListInterfaces[]          = "ListInterfaces";      ///< 返回所有可用接口名的字符串数组
static constexpr const char kMethodGetInterfaces[]           = "GetInterfaces";      ///< ListInterfaces 的同义名（向后兼容）
static constexpr const char kMethodHealthCheck[]            = "HealthCheck";        ///< 执行网络健康检查，返回 JSON 格式诊断结果
static constexpr const char kMethodPing[]                   = "Ping";               ///< 对指定主机执行 ICMP Ping，返回延迟和丢包
static constexpr const char kMethodGetBluetoothDevices[]    = "GetBluetoothDevices";///< 返回已发现蓝牙设备列表（BlueZ D-Bus）
static constexpr const char kMethodGetBluetoothAdapter[]     = "GetBluetoothAdapter";///< 返回蓝牙适配器状态（是否可用、是否在连接中）
static constexpr const char kMethodGetDnsStats[]            = "GetDnsStats";        ///< 查询 DNS eBPF 监控的最近统计（平均延迟、超时率）
static constexpr const char kMethodGetWifiLossStats[]        = "GetWifiLossStats";   ///< 查询 Wi-Fi eBPF 丢包归因（按 ifindex）
static constexpr const char kMethodGetHttpLatencyStats[]    = "GetHttpLatencyStats";///< 查询 HTTP eBPF 请求延迟统计（p50/p99/慢请求）
static constexpr const char kMethodGetProcessProfiling[]    = "GetProcessProfiling";///< 查询 eBPF 进程网络画像（Top N 带宽进程）
static constexpr const char kMethodGetEbpfMonitorHealth[]   = "GetEbpfMonitorHealth";///< 查询所有 eBPF 监控器健康状态（是否成功加载）
static constexpr const char kMethodGetHistory[]              = "GetHistory";         ///< 查询 SQLite 历史快照（支持时间范围和网卡过滤）
static constexpr const char kMethodSetMonitorParam[]         = "SetMonitorParam";   ///< 运行时设置监控器参数（白名单校验+原子提交）
static constexpr const char kMethodGetMonitorParam[]         = "GetMonitorParam";   ///< 查询监控器当前参数（JSON 格式）

// ==================== 信号名 ====================
// 服务端主动向订阅客户端推送事件。客户端通过 dbus_bus_add_match 过滤感兴趣的信号

static constexpr const char kSignalChanged[]                = "Changed";                ///< 通用变化信号（文本消息 + 计数器，旧版兼容）
static constexpr const char kSignalInterfaceChanged[]       = "InterfaceChanged";       ///< 网卡添加/删除事件
static constexpr const char kSignalConnectionModeChanged[]  = "ConnectionModeChanged";  ///< 当前上网网卡切换事件
static constexpr const char kSignalNetworkQualityChanged[]  = "NetworkQualityChanged";  ///< 综合网络质量（Excellent/Good/Fair/Poor）变化
static constexpr const char kSignalBluetoothDeviceChanged[] = "BluetoothDeviceChanged"; ///< 蓝牙设备出现/消失/状态变化

// ==================== 运行时路径常量 ====================

/// SQLite 历史数据持久化路径解析函数。
/// 见 kDatabasePath 注释。
inline std::string resolveDatabasePath(const std::string& cfg_data_dir) {
    if (!cfg_data_dir.empty()) {
        return cfg_data_dir + "/history.db";
    }
    const char* env = std::getenv("WEAKNET_DATA_DIR");
    if (env && *env) {
        return std::string(env) + "/history.db";
    }
    return std::string("/home/radxa/weaknet/data") + "/history.db";
}

/// 默认流量分析接口名。空字符串 "" 表示自动选择当前活动接口
/// 可通过 WEAKNET_TRAFFIC_IFACE 覆盖（如 WEAKNET_TRAFFIC_IFACE=eth1 强制分析指定网卡）
inline const std::string kDefaultTrafficInterface = []() {
    const char* env = std::getenv("WEAKNET_TRAFFIC_IFACE");
    return env ? env : "";
}();

/// Changed 信号序列化输出文件路径
/// 使用 $XDG_RUNTIME_DIR 私有目录（通常是 /run/user/$UID），防符号链接攻击
/// 若 XDG_RUNTIME_DIR 未设置则退回到 /tmp（开发环境兼容）
inline const std::string kSignalSerializedFile = []() {
    const char* xdg = std::getenv("XDG_RUNTIME_DIR");
    return std::string(xdg ? xdg : "/tmp") + "/weaknet/signal_changed.bin";
}();

/// Get 方法回复序列化输出文件路径（同安全策略）
inline const std::string kGetReplySerializedFile = []() {
    const char* xdg = std::getenv("XDG_RUNTIME_DIR");
    return std::string(xdg ? xdg : "/tmp") + "/weaknet/get_reply.bin";
}();

}  // namespace weaknet_dbus
