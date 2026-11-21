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

#include <cstdint>
#include <cstdlib>
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

static constexpr const char kMethodListInterfaces[]          = "ListInterfaces";      ///< 返回所有可用接口名的字符串数组
static constexpr const char kMethodGetInterfaces[]           = "GetInterfaces";      ///< ListInterfaces 的同义名（向后兼容）
static constexpr const char kMethodHealthCheck[]            = "HealthCheck";        ///< 执行网络健康检查，返回 JSON 格式诊断结果
static constexpr const char kMethodGetDiagnosis[]          = "GetDiagnosis";       ///< 查询端侧确定性机器诊断事实（DiagnosisFacts JSON）
static constexpr const char kMethodExecuteAction[]          = "ExecuteAction";      ///< 通过 ActionRegistry 执行安全动作（posix_spawn 隔离）
static constexpr const char kMethodPing[]                   = "Ping";               ///< 对指定主机执行 ICMP Ping，返回延迟和丢包
static constexpr const char kMethodGetBluetoothDevices[]    = "GetBluetoothDevices";///< 返回已发现蓝牙设备列表（BlueZ D-Bus）
static constexpr const char kMethodGetBluetoothAdapter[]     = "GetBluetoothAdapter";///< 返回蓝牙适配器状态（是否可用、是否在连接中）
static constexpr const char kMethodGetBluetoothAudioQuality[] = "GetBluetoothAudioQuality"; ///< 查询蓝牙 A2DP 音频质量与 eBPF 融合诊断（JSON）
static constexpr const char kMethodGetCoexistenceConflict[]  = "GetCoexistenceConflict";
static constexpr const char kMethodGetNetworkExperience[] = "GetNetworkExperience";   ///< 查询 Wi-Fi 与蓝牙 2.4GHz 共存冲突与根因分析（JSON）
static constexpr const char kMethodGetDnsStats[]            = "GetDnsStats";        ///< 查询 DNS eBPF 监控的最近统计（平均延迟、超时率）
static constexpr const char kMethodGetWifiLossStats[]        = "GetWifiLossStats";   ///< 查询 Wi-Fi eBPF 丢包归因（按 ifindex）
static constexpr const char kMethodGetHttpLatencyStats[]    = "GetHttpLatencyStats";///< 查询 HTTP eBPF 请求延迟统计（p50/p99/慢请求）
static constexpr const char kMethodGetProcessProfiling[]    = "GetProcessProfiling";///< 查询 eBPF 进程网络画像（Top N 带宽进程）
static constexpr const char kMethodGetSkbDropStats[]        = "GetSkbDropStats";    ///< 查询 Socket/skb 丢包原因精确归因统计（JSON）
static constexpr const char kMethodGetTcpRttStats[]         = "GetTcpRttStats";     ///< 查询被动 TCP RTT 与拥塞统计（JSON）
static constexpr const char kMethodGetTcpRetransStats[]     = "GetTcpRetransStats"; ///< 查询 TCP 连接级重传统计（JSON）
static constexpr const char kMethodGetTcpConnStats[]        = "GetTcpConnStats";    ///< 查询 TCP 连接生命周期统计（JSON）
static constexpr const char kMethodGetEbpfMonitorHealth[]   = "GetEbpfMonitorHealth";///< 查询所有 eBPF 监控器健康状态（是否成功加载）
static constexpr const char kMethodGetHistory[]              = "GetHistory";         ///< 查询 SQLite 历史快照（支持时间范围和网卡过滤）
static constexpr const char kMethodQueryDeviceEvents[]      = "QueryDeviceEvents";  ///< 查询规范化无线设备事件（时间范围/设备/类型过滤）
static constexpr const char kMethodQueryBtDataPath[]        = "QueryBtDataPath";    ///< 查询工业蓝牙数传链路流量指标（Phase 1b：时间/设备过滤）
static constexpr const char kMethodQuerySiteIncidents[]     = "QuerySiteIncidents"; ///< 查询区域级异常事件 SiteIncident（Phase 3a：区域层）
static constexpr const char kMethodSetMonitorParam[]         = "SetMonitorParam";   ///< 运行时设置监控器参数（白名单校验+原子提交）
static constexpr const char kMethodGetMonitorParam[]         = "GetMonitorParam";   ///< 查询监控器当前参数（JSON 格式）
static constexpr const char kMethodListMonitors[]             = "ListMonitors";
static constexpr const char kMethodGetMonitorStatus[]         = "GetMonitorStatus";
static constexpr const char kMethodEnableMonitor[]            = "EnableMonitor";
static constexpr const char kMethodDisableMonitor[]           = "DisableMonitor";
static constexpr const char kMethodRestartMonitor[]           = "RestartMonitor";
static constexpr const char kMethodSaveMonitorOverrides[]     = "SaveMonitorOverrides";
static constexpr const char kMethodGetUpdateStatus[]          = "GetUpdateStatus";    ///< OTA 本地升级状态（只读：current 软链接版本 / 布局 / 完整性）
static constexpr const char kMethodGetVideoFrame[]            = "GetVideoFrame";      ///< 取最新摄像头帧的 dma-buf 句柄（UNIX_FD + 几何 + sequence）
static constexpr const char kMethodReleaseVideoFrame[]        = "ReleaseVideoFrame";  ///< 归还 GetVideoFrame 拿到的槽位（不归还则等超时回收）

// ==================== 信号名 ====================
// 服务端主动向订阅客户端推送事件。客户端通过 dbus_bus_add_match 过滤感兴趣的信号

static constexpr const char kSignalChanged[]                = "Changed";                ///< 通用变化信号（文本消息 + 计数器，旧版兼容）
static constexpr const char kSignalInterfaceChanged[]       = "InterfaceChanged";       ///< 网卡添加/删除事件
static constexpr const char kSignalConnectionModeChanged[]  = "ConnectionModeChanged";  ///< 当前上网网卡切换事件
static constexpr const char kSignalNetworkQualityChanged[]  = "NetworkQualityChanged";  ///< 综合网络质量（Excellent/Good/Fair/Poor）变化
static constexpr const char kSignalBluetoothDeviceChanged[] = "BluetoothDeviceChanged"; ///< 蓝牙设备出现/消失/状态变化
static constexpr const char kSignalMonitorStateChanged[]    = "MonitorStateChanged";
///< eBPF 监控器聚合指标推送（JSON：monitor + 该监控器的关键统计 + 驱逐差分）
static constexpr const char kSignalEbpfMonitorStats[]       = "EbpfMonitorStats";

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

/// 网络代次状态文件路径。与 history.db 同目录 —— 二者都是"这台设备的
/// 持久身份"，放在一起才能保证备份/迁移时不会只带走一半。
inline std::string resolveNetworkEpochPath(const std::string& cfg_data_dir) {
    return resolveDatabasePath(cfg_data_dir).substr(
               0, resolveDatabasePath(cfg_data_dir).size() - std::string("history.db").size()) +
           "network-epoch";
}

/// 事故静帧留证目录。与 history.db 同目录 —— 事故图是取证材料，必须和
/// 事故记录（device_events / site_incidents）待在一起，否则现场拷走一半。
///
/// 落在持久分区而不是 /tmp：/tmp 在板端是 tmpfs，重启即失；而事故恰恰
/// 经常伴随重启（看门狗复位、掉电），最需要留下的那几张图会正好丢掉。
inline std::string resolveSnapshotDir(const std::string& cfg_data_dir) {
    return resolveDatabasePath(cfg_data_dir).substr(
               0, resolveDatabasePath(cfg_data_dir).size() - std::string("history.db").size()) +
           "snapshots";
}

/// 事故静帧目录的总字节上限。0 = 不回收。
///
/// 为什么是环境变量而不是 config.yaml 键：上限的语义是「这块盘还剩多少」，
/// 是**部署事实**而不是运行期可调参数。它不该出现在 `weaknet-cli set` 的
/// 白名单里被实时改小——那会让一次误操作开始删本机唯一的证据副本；也不该
/// 进 D-Bus 事务回滚（回滚一个已经删掉的文件是不可能的）。
///
/// 默认 0（关闭）而不是一个数字：没人复核过的数字不该开始删证据。与云端
/// `SNAPSHOT_RETENTION_DAYS` 没有默认值是同一个理由。
///
/// 默认建议值是 2 GiB —— 单张 JPEG 实测 118~128 KB，2 GiB 约合 1.6 万张；
/// 而本板 `/home` 有 117 GB，2 GiB 只占 1.7%，不会因为「拍证」把盘写满。
/// 但它**不是默认值**，只是注释里的建议，部署时必须显式给出。
inline uint64_t resolveSnapshotMaxBytes() {
    const char* env = std::getenv("WEAKNET_SNAPSHOT_MAX_BYTES");
    if (!env || !*env) return 0;
    // 只接受纯十进制数字。带单位的字符串（"2G"）需要一套解析规则与边界
    // 校验，而这条路径删的是证据——宁可让人写成 2147483648 这个明确的数字，
    // 也不要在这里猜 unit 后缀。
    uint64_t value = 0;
    for (const char* p = env; *p; ++p) {
        if (*p < '0' || *p > '9') return 0;
        if (value > (UINT64_MAX - 9) / 10) return 0;  // 溢出
        value = value * 10 + static_cast<uint64_t>(*p - '0');
    }
    return value;
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

}  // namespace weaknet_dbus
