/**
 * @file bt_monitor.hpp
 * @brief 蓝牙监测器：通过 BlueZ D-Bus API 监测蓝牙适配器与附近设备
 *
 * 功能概览：
 *   - 适配器状态监控（电源、可发现、UUID 等）
 *   - 设备发现与 RSSI 跟踪（维护最近 30 次 RSSI 历史）
 *   - 连接/配对/信任状态事件
 *   - A2DP 音频传输状态（MediaTransport1 接口）
 *   - 基于 RSSI 路径损耗模型的距离估算
 *   - Phase 2 eBPF 融合层（可选，用于更精准的音频质量评估）
 *
 * 架构：
 *   BtMonitor 持有 BlueZ 系统总线连接（DBusConnection*），
 *   工作线程每 3 秒刷新一次设备状态（refreshDeviceStates）。
 *   事件队列（pendingEvents_）由工作线程写入、主线程通过 fetchEvents() 消费。
 *
 * 线程安全：
 *   - adapterMutex_    保护 adapterState_ / adapterPath_
 *   - deviceMutex_     保护 devices_ 映射
 *   - eventMutex_      保护 pendingEvents_ 队列
 *   - audioMutex_      保护 audioTransports_ 映射与 MediaTransport1 探测缓存
 *   - initialized_ / running_ 使用 std::atomic，lock-free 安全
 *
 * 降级策略：
 *   - 无蓝牙适配器 / BlueZ 未运行 → initialize() 返回 false，后续所有查询返回空
 *   - MediaTransport1 接口缺失 → hasMediaTransportInterface() 探测一次缓存结果，
 *                                 仅使用 Phase 1b D-Bus 状态评分
 *   - eBPF 融合层失败 → initPhase2() 返回 false，自动降级为纯 D-Bus 模式
 */

#pragma once

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>

// 前置声明 libdbus 类型
struct DBusConnection;
struct DBusMessage;
struct DBusError;

// 前置声明测试友元（全局命名空间，供单元测试访问 calculateAudioScore 纯函数）
class BtMonitorAudioScoreTest;

// 前置声明 Phase 2 eBPF 融合层类型
namespace weaknet_dbus {
class BtAudioAnalyzer;
struct BtAudioFusionResult;
class BtAudioFusion;
struct BtTrafficStats;
}

namespace weaknet_dbus {

// ============================================================================
// 蓝牙设备类型
// ============================================================================
enum class BtDeviceType {
    Unknown = 0,   ///< 类型未知（未读取到设备信息）
    Classic,       ///< 经典蓝牙 (BR/EDR)，如传统耳机、键盘
    BLE,           ///< 低功耗蓝牙，如智能手环、Beacon
    Dual,          ///< 双模设备，同时支持 Classic 和 BLE
};

// ============================================================================
// 蓝牙设备信息
// ============================================================================
struct BtDeviceInfo {
    std::string macAddress;          ///< 设备 MAC 地址 (如 "AA:BB:CC:DD:EE:FF")
    std::string name;                ///< 设备名称 (可能为空，需要额外查询)
    std::string alias;               ///< 设备别名 (用户自定义名称)
    int16_t rssiDbm = -1000;         ///< 信号强度 dBm，-1000 表示未获取（哨兵值）
    int16_t txPower = 0;             ///< 发射功率 dBm
    bool connected = false;          ///< 是否已连接
    bool paired = false;             ///< 是否已配对
    bool trusted = false;            ///< 是否受信任（自动允许连接）
    bool blocked = false;            ///< 是否被屏蔽
    bool legacyPairing = false;      ///< 是否使用传统配对（非 SSP）
    BtDeviceType deviceType = BtDeviceType::Unknown;
    uint16_t appearance = 0;         ///< BLE Appearance 特征值（设备类别标识符）
    std::vector<std::string> uuids;  ///< 服务 UUID 列表
    std::string manufacturerData;    ///< 厂商数据 (hex 编码)
    std::string icon;                ///< 设备图标名称（BlueZ 提供，如 "audio-headset"）
    double estimatedDistance = -1.0; ///< 估算距离（米），-1.0 表示未知/未估算（哨兵值）
    int16_t calibratedTxPower = -59; ///< 校准后的 1 米参考 RSSI (dBm)，默认 -59
    std::chrono::system_clock::time_point lastSeen;    ///< 最近一次被发现的时间
    std::chrono::system_clock::time_point lastUpdated;  ///< 最近一次属性刷新的时间

    // ---- 历史数据 (用于趋势分析) ----
    std::vector<int16_t> rssiHistory;      ///< 最近 30 次 RSSI 记录（环形追加）
    static constexpr size_t MAX_RSSI_HISTORY = 30;  ///< 历史队列容量上限

    /**
     * @brief 返回 RSSI 平均值
     * @return 平均 RSSI（dBm）；若无线记录返回 0
     */
    int16_t averageRssi() const {
        if (rssiHistory.empty()) return 0;
        int32_t sum = 0;
        for (auto r : rssiHistory) sum += r;
        return static_cast<int16_t>(sum / static_cast<int32_t>(rssiHistory.size()));
    }

    /**
     * @brief 返回 RSSI 信号等级字符串
     *
     * 分级依据实际测试环境距离估算（参考 RSSI 路径损耗模型的典型值）。
     * @return "unknown" / "excellent" / "good" / "fair" / "poor" / "very_poor"
     */
    std::string rssiLevel() const {
        int16_t r = averageRssi();
        if (r == 0) return "unknown";
        if (r >= -50) return "excellent";   // 极近（<1米）
        if (r >= -60) return "good";         // 1-3米
        if (r >= -70) return "fair";         // 3-7米
        if (r >= -80) return "poor";         // 7-12米
        return "very_poor";                   // >12米/接近断连
    }
};

// ============================================================================
// 蓝牙音频传输状态（对应 BlueZ org.bluez.MediaTransport1 接口）
// ============================================================================
struct BtAudioTransport {
    std::string transportPath;      ///< MediaTransport1 对象路径（如 /org/bluez/hci0/dev_XX/transport1）
    std::string deviceMac;          ///< 关联设备 MAC 地址
    std::string state;              ///< Transport 状态: active / inactive / idle / pending
    uint16_t delay = 0;             ///< 音频延迟，单位 1/10ms（BlueZ 上报值）
    uint16_t volume = 0;            ///< 音量 0-127
    uint8_t codec = 0;              ///< 编解码器 ID (0x00=SBC, 0x01=MPEG12, 0x02=MPEG24, 0x04=ATRAC...)
    std::chrono::system_clock::time_point lastActive;      ///< 最后一次变为 active 的时间
    uint64_t activeDurationMs = 0;  ///< 累计活跃时长（毫秒）
};

// ============================================================================
// 蓝牙音频质量评估结果（Phase 1b 输出）
// ============================================================================
struct BtAudioQuality {
    std::string deviceMac;         ///< 评估对象的设备 MAC
    bool isActive = false;         ///< 当前 D-Bus 报告的 transport 是否 active
    double qualityScore = 0.0;     ///< 0-100 质量分数（越高越好）
    std::string level;             ///< excellent / good / fair / poor / unknown
    uint16_t currentDelay = 0;     ///< 当前延迟（1/10ms，来自 Transport 属性）
    double activeRatio = 0.0;      ///< 活跃占比 = activeDurationMs / (refresh 周期)
    std::vector<std::string> issues; ///< 诊断问题列表（延迟过高、频繁切换等）
};

// ============================================================================
// 蓝牙适配器状态（对应 BlueZ org.bluez.Adapter1 接口）
// ============================================================================
struct BtAdapterState {
    std::string macAddress;          ///< 适配器 MAC 地址
    std::string name;                ///< 适配器显示名称
    std::string alias;               ///< 别名
    bool powered = false;            ///< 是否已开启蓝牙
    bool discovering = false;        ///< 是否正在扫描设备
    bool discoverable = false;       ///< 是否可被其他设备发现
    bool pairable = false;           ///< 是否允许配对
    uint32_t deviceClass = 0;        ///< 设备类型 (Class of Device 3-bit 编码)
    std::vector<std::string> uuids;  ///< 适配器支持的 Profile UUID 列表
};

// ============================================================================
// 蓝牙事件数据（由 BtMonitor 内部事件队列广播给上层）
// ============================================================================
struct BtEvent {
    /// 事件类型枚举；与事件字段填充有严格对应关系
    enum class Type {
        AdapterAdded,        ///< 适配器插入 (USB 蓝牙适配器热插拔)
        AdapterRemoved,      ///< 适配器移除
        AdapterPowered,      ///< 蓝牙开关状态改变
        DeviceFound,         ///< 发现新设备
        DeviceLost,          ///< 设备离开扫描范围
        DeviceConnected,     ///< 设备已连接
        DeviceDisconnected,  ///< 设备断开
        DeviceRssiChanged,   ///< 设备 RSSI 显著变化（>6dBm）
        DiscoveryStarted,    ///< 扫描开始
        DiscoveryStopped,    ///< 扫描停止
    };

    Type type;
    std::string adapterMac;         ///< 所属适配器 MAC
    std::string deviceMac;          ///< 关联设备 MAC（若事件涉及设备）
    std::string deviceName;         ///< 设备名称（便于日志和上层消费）
    int16_t rssiDbm = -1000;        ///< 当前 RSSI（若适用，哨兵值 -1000 表示不携带）
    std::string message;            ///< 人类可读描述，方便日志打印
    std::chrono::system_clock::time_point timestamp;
};

// ============================================================================
// 蓝牙监测器类
// ============================================================================
class BtMonitor {
public:
    BtMonitor();
    ~BtMonitor();

    // ---- 生命周期 ----

    /**
     * @brief 初始化：连接系统总线上的 BlueZ 服务
     * @return true  成功连接并找到至少一个蓝牙适配器
     * @return false 无蓝牙适配器 / BlueZ 不可用 / D-Bus 连接失败
     */
    bool initialize();

    /**
     * @brief 清理资源：释放 DBusConnection、停止工作线程
     */
    void cleanup();

    /// 是否已完成 initialize() 且未 cleanup()
    bool isInitialized() const { return initialized_.load(); }

    /// 是否至少存在一个蓝牙适配器（从 adapterState_ 判断）
    bool hasAdapter() const;

    // ----- 适配器操作 -----

    /// 获取适配器当前状态（adapterMutex_ 保护）
    BtAdapterState getAdapterState() const;

    /// 设置适配器电源开关；通过 BlueZ Set Powered 方法
    bool setPowered(bool on);

    /// 开启扫描（BlueZ StartDiscovery）
    bool startDiscovery();

    /// 停止扫描（BlueZ StopDiscovery）
    bool stopDiscovery();

    // ----- 设备查询 -----

    /// 获取所有已知设备副本（deviceMutex_ 保护）
    std::vector<BtDeviceInfo> getDevices() const;

    /**
     * @brief 根据 MAC 获取单个设备信息
     * @param mac  设备 MAC 地址
     * @param out  输出参数
     * @return true 找到并写入 out；false 设备不存在
     */
    bool getDevice(const std::string& mac, BtDeviceInfo* out) const;

    /// 获取已连接设备列表（connected=true 的子集）
    std::vector<BtDeviceInfo> getConnectedDevices() const;

    /**
     * @brief 获取附近设备
     * @param maxAgeSec  只返回最近 N 秒内被发现的设备，默认 30 秒
     */
    std::vector<BtDeviceInfo> getNearbyDevices(int maxAgeSec = 30) const;

    // ----- RSSI 查询 -----

    /// 获取指定设备的当前 RSSI（若设备不存在返回哨兵值 -1000）
    int16_t getDeviceRssi(const std::string& mac) const;

    /// 获取所有设备的 RSSI 快照（MAC → RSSI 映射）
    std::map<std::string, int16_t> getRssiSnapshot() const;

    // ----- 事件获取 -----

    /**
     * @brief 获取并清空待处理事件（线程安全）
     * @return 事件列表的拷贝；内部队列同时被清空
     */
    std::vector<BtEvent> fetchEvents();

    // ----- 统计信息 -----

    size_t deviceCount() const;      ///< 当前已知设备总数
    size_t connectedCount() const;   ///< 当前已连接设备数

    // ----- A2DP 音频质量监控（Phase 1b）-----

    /**
     * @brief 刷新 MediaTransport1 音频传输状态
     *
     * 主动查询所有 BlueZ MediaTransport1 对象，更新 audioTransports_ 映射。
     * 每次调用都会探测接口是否存在（hasMediaTransportInterface 缓存结果）。
     */
    void refreshAudioTransports();

    /**
     * @brief 查询指定设备的音频质量
     * @param mac 设备 MAC 地址
     * @param out 输出参数，接收质量评估结果
     * @return true 若找到该设备的 Transport 信息
     */
    bool getAudioQuality(const std::string& mac, BtAudioQuality* out) const;

    /**
     * @brief 检查 BlueZ 是否支持 MediaTransport1 接口
     *
     * 只探测一次，结果缓存到 hasMediaTransport_（mediaMutex_ 保护）。
     * 用于上层判断能否启用音频监控降级策略。
     */
    bool hasMediaTransportInterface();

    /// 获取所有音频传输状态副本（audioMutex_ 保护）
    std::vector<BtAudioTransport> getAudioTransports() const;

    // ----- 设备距离估算（Phase 1b）-----

    /**
     * @brief 基于 RSSI 路径损耗模型估算设备距离
     *
     * 使用对数-路径损耗模型：
     *   distance = REFERENCE_DISTANCE_M * 10^((calibratedTxPower - rssiDbm) / (10 * PATH_LOSS_EXPONENT))
     *
     * @param rssiDbm  当前 RSSI 值 (dBm)；无效值（=0 或 <= -1000）返回 -1.0
     * @return 估算距离（米），无效输入返回 -1.0
     */
    double estimateDistance(int16_t rssiDbm) const;

    /**
     * @brief 校准设备距离参考点
     *
     * 将当前 RSSI 作为 knownMeters 距离的参考 RSSI，
     * 后续 estimateDistance 调用将使用该值代替 defaultTxPower_。
     *
     * @param mac         设备 MAC 地址
     * @param knownMeters 已知距离（米），通常为 1.0
     * @return true 若设备存在且校准成功
     */
    bool calibrateDistance(const std::string& mac, double knownMeters);

    /**
     * @brief 设置默认发射功率（1 米参考 RSSI）
     * @param txPower 1 米处的 RSSI 值 (dBm)，默认 -59（室内环境典型值）
     */
    void setDefaultTxPower(int16_t txPower);

    // ----- Phase 2: eBPF 融合层 -----

    /**
     * @brief 初始化 eBPF 融合层
     *
     * 加载 a2dp_media.bpf.o 并挂载 kprobe 到蓝牙 L2CAP 发送路径。
     * 任一挂点成功即标记可用；全部失败则降级为纯 D-Bus 模式。
     *
     * @param bpfObjectPath BPF 目标文件路径，如 "build/a2dp_media.bpf.o"
     * @return true 若至少一个挂点成功；false 全部失败（用户空间不崩溃，
     *         后续音频质量自动退化为 Phase 1b 逻辑）
     */
    bool initPhase2(const std::string& bpfObjectPath);

    /// 停止 eBPF 融合层（释放内核资源）
    void stopPhase2();

    /**
     * @brief 获取融合评估结果（Phase 2）
     * @param mac 设备 MAC 地址
     * @param out 输出参数，接收融合结果
     * @return true 若找到该设备的音频数据
     */
    bool getAudioFusionResult(const std::string& mac, BtAudioFusionResult* out) const;

    /// 获取音频 eBPF 分析器指针，供统一监控健康查询使用
    BtAudioAnalyzer* audioAnalyzer() const { return btAudioAnalyzer_.get(); }

    /// eBPF 融合层是否可用（至少一个挂点成功）
    bool isPhase2Available() const;

    // ----- 周期刷新 (由工作线程调用) -----

    /// 执行一轮设备状态刷新：listDevicePaths → 逐设备 parseDeviceProperties
    void refreshDeviceStates();

    /// 获取适配器属性（使用内部 sysConn_），adapterMutex_ 保护
    bool refreshAdapterState();

private:
    // ---- D-Bus 辅助方法 ----

    /// 解析 BlueZ 返回的设备属性（org.bluez.Device1 接口）
    BtDeviceInfo parseDeviceProperties(DBusConnection* sysConn,
                                        const std::string& devPath);

    /// 获取 BlueZ 适配器下的所有设备对象路径（ListManagedObjects）
    std::vector<std::string> listDevicePaths(DBusConnection* sysConn);

    /// 通过 D-Bus 读取对象属性（org.freedesktop.DBus.Properties Get）
    std::string getStringProperty(DBusConnection* conn,
                                   const std::string& objPath,
                                   const std::string& iface,
                                   const std::string& propName);
    int16_t getInt16Property(DBusConnection* conn,
                             const std::string& objPath,
                             const std::string& iface,
                             const std::string& propName);
    bool getBoolProperty(DBusConnection* conn,
                        const std::string& objPath,
                        const std::string& iface,
                        const std::string& propName);
    std::vector<std::string> getStringArrayProperty(DBusConnection* conn,
                                                     const std::string& objPath,
                                                     const std::string& iface,
                                                     const std::string& propName);

    /// 调用 BlueZ 无返回值方法（如 StartDiscovery / StopDiscovery）
    bool callBlueZMethod(const std::string& objPath,
                         const std::string& iface,
                         const std::string& method);

    /// 发送 D-Bus 消息并获取回复；超时默认 3000ms
    DBusMessage* sendWithReply(DBusConnection* conn, DBusMessage* msg, int timeoutMs = 3000);

    // ---- A2DP 音频监控辅助 ----

    /// 列出所有 MediaTransport1 对象路径（ListManagedObjects 过滤）
    std::vector<std::string> listMediaTransportPaths();

    /// 解析单个 MediaTransport1 对象的属性
    BtAudioTransport parseMediaTransportProperties(const std::string& path);

    /// 计算音频质量评分（纯函数，基于 Transport 状态，无副作用）
    double calculateAudioScore(const BtAudioTransport& transport) const;

    /// 数值评分 → 等级字符串
    static std::string scoreToLevel(double score);

    /// 测试友元：单元测试需要访问 calculateAudioScore 纯函数以验证评分边界
    friend class ::BtMonitorAudioScoreTest;

private:
    // ---- D-Bus 连接 ----
    DBusConnection* sysConn_ = nullptr;  ///< 系统总线连接（BlueZ 在系统总线上）

    // ---- 运行状态 ----
    std::atomic<bool> initialized_{false};  ///< initialize() 是否成功
    std::atomic<bool> running_{false};      ///< 工作线程是否在运行

    std::unique_ptr<std::thread> workerThread_;  ///< 后台刷新线程

    // ---- 适配器状态 ----
    mutable std::mutex adapterMutex_;   ///< 保护 adapterState_ / adapterPath_
    BtAdapterState adapterState_;       ///< 当前适配器状态
    std::string adapterPath_;            ///< BlueZ 对象路径，如 "/org/bluez/hci0"

    // ---- 设备列表 ----
    mutable std::mutex deviceMutex_;    ///< 保护 devices_ 映射
    std::map<std::string, BtDeviceInfo> devices_;  ///< MAC → 设备信息

    // ---- 事件队列 ----
    mutable std::mutex eventMutex_;     ///< 保护 pendingEvents_
    std::vector<BtEvent> pendingEvents_;  ///< 待处理事件（fetchEvents 消费）

    // ---- 扫描节流 ----
    std::chrono::system_clock::time_point lastDiscoveryStart_;  ///< 上次扫描开始时间
    static constexpr int DISCOVERY_TIMEOUT_SEC = 30;  ///< 扫描持续时间上限

    // ---- 刷新频率 ----
    static constexpr int REFRESH_INTERVAL_SEC = 3;    ///< 设备刷新间隔

    // ---- A2DP 音频监控状态 ----
    mutable std::mutex audioMutex_;
    std::map<std::string, BtAudioTransport> audioTransports_;  ///< MAC → Transport 状态
    mutable bool mediaTransportProbed_ = false;  ///< 是否已探测 MediaTransport1 接口
    mutable bool hasMediaTransport_ = false;     ///< 探测结果缓存（true=存在）
    static constexpr const char* MEDIA_IFACE = "org.bluez.MediaTransport1";

    // ---- 距离估算参数 ----
    int16_t defaultTxPower_ = -59;                   ///< 默认 1 米参考 RSSI (dBm)
    static constexpr double PATH_LOSS_EXPONENT = 2.5; ///< 室内路径损耗指数（自由空间为 2.0）
    static constexpr double REFERENCE_DISTANCE_M = 1.0; ///< 参考距离（米）

    // ---- Phase 2: eBPF 融合层 ----
    std::unique_ptr<BtAudioAnalyzer> btAudioAnalyzer_;          ///< eBPF 分析器
    std::unique_ptr<BtAudioFusion> btAudioFusion_;              ///< 融合评估器
    mutable std::map<std::string, BtTrafficStats> btPrevStats_; ///< 前次 eBPF 统计快照 (MAC→prev)
    static constexpr const char* BPF_OBJECT_PATH = "build/a2dp_media.bpf.o";
};

// ============================================================================
// 启动蓝牙监测线程 (在 server.cpp 中调用)
// ============================================================================
struct ServerContext;

/**
 * @brief 启动蓝牙监测线程
 *
 * 线程循环：initialize() → 每 REFRESH_INTERVAL_SEC 秒 refreshDeviceStates() + refreshAudioTransports()。
 * outMonitor 回传 BtMonitor 实例指针（调用方不拥有所有权，由线程内部 shared_ptr 管理），
 * 供 DbusService 查询。
 *
 * @param ctx        ServerContext 生命周期句柄
 * @param outMonitor 可选输出；若非 nullptr，接收 BtMonitor* 指针
 */
void start_bt_monitor_thread(ServerContext* ctx, BtMonitor** outMonitor = nullptr);

}  // namespace weaknet_dbus
