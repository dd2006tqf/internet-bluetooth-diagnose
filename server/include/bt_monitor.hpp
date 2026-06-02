// bt_monitor.hpp
// 蓝牙监测器：通过 BlueZ D-Bus API 监测蓝牙适配器与附近设备
// 支持：适配器状态、设备发现、RSSI跟踪、连接状态变化

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

namespace weaknet_dbus {

// ============================================================================
// 蓝牙设备类型
// ============================================================================
enum class BtDeviceType {
    Unknown = 0,
    Classic,       // 经典蓝牙 (BR/EDR)
    BLE,           // 低功耗蓝牙
    Dual,          // 双模设备
};

// ============================================================================
// 蓝牙设备信息
// ============================================================================
struct BtDeviceInfo {
    std::string macAddress;          // 设备 MAC 地址 (如 "AA:BB:CC:DD:EE:FF")
    std::string name;                // 设备名称 (可能为空，需要额外查询)
    std::string alias;               // 设备别名 (用户自定义名称)
    int16_t rssiDbm = -1000;         // 信号强度 dBm (0 表示未获取)
    int16_t txPower = 0;             // 发射功率 dBm
    bool connected = false;          // 是否已连接
    bool paired = false;             // 是否已配对
    bool trusted = false;            // 是否受信任
    bool blocked = false;            // 是否被屏蔽
    bool legacyPairing = false;      // 是否使用传统配对
    BtDeviceType deviceType = BtDeviceType::Unknown;
    uint16_t appearance = 0;         // BLE Appearance 特征值
    std::vector<std::string> uuids;  // 服务 UUID 列表
    std::string manufacturerData;    // 厂商数据 (hex)
    std::string icon;                // 设备图标名称
    std::chrono::system_clock::time_point lastSeen;
    std::chrono::system_clock::time_point lastUpdated;

    // 历史数据 (用于趋势分析)
    std::vector<int16_t> rssiHistory;      // 最近 30 次 RSSI 记录
    static constexpr size_t MAX_RSSI_HISTORY = 30;

    // 返回 RSSI 平均值 (若无线记录返回 0)
    int16_t averageRssi() const {
        if (rssiHistory.empty()) return 0;
        int32_t sum = 0;
        for (auto r : rssiHistory) sum += r;
        return static_cast<int16_t>(sum / static_cast<int32_t>(rssiHistory.size()));
    }

    // 返回 RSSI 信号等级
    std::string rssiLevel() const {
        int16_t r = averageRssi();
        if (r == 0) return "unknown";
        if (r >= -50) return "excellent";   // 极近
        if (r >= -60) return "good";         // 1-3米
        if (r >= -70) return "fair";         // 3-7米
        if (r >= -80) return "poor";         // 7-12米
        return "very_poor";                   // >12米/接近断连
    }
};

// ============================================================================
// 蓝牙适配器状态
// ============================================================================
struct BtAdapterState {
    std::string macAddress;          // 适配器 MAC 地址
    std::string name;                // 适配器显示名称
    std::string alias;               // 别名
    bool powered = false;            // 是否已开启
    bool discovering = false;        // 是否正在扫描
    bool discoverable = false;       // 是否可被发现
    bool pairable = false;           // 是否可配对
    uint32_t deviceClass = 0;        // 设备类型 (Class of Device)
    std::vector<std::string> uuids;  // 适配器支持的 UUID
};

// ============================================================================
// 蓝牙事件数据
// ============================================================================
struct BtEvent {
    enum class Type {
        AdapterAdded,        // 适配器插入 (USB蓝牙适配器等)
        AdapterRemoved,      // 适配器移除
        AdapterPowered,      // 蓝牙开关状态改变
        DeviceFound,         // 发现新设备
        DeviceLost,          // 设备离开范围
        DeviceConnected,     // 设备已连接
        DeviceDisconnected,  // 设备断开
        DeviceRssiChanged,   // 设备 RSSI 显著变化 (>6dBm)
        DiscoveryStarted,    // 扫描开始
        DiscoveryStopped,    // 扫描停止
    };

    Type type;
    std::string adapterMac;         // 所属适配器
    std::string deviceMac;          // 关联设备 (若适用)
    std::string deviceName;         // 设备名称
    int16_t rssiDbm = -1000;        // 当前 RSSI (若适用)
    std::string message;            // 人类可读消息
    std::chrono::system_clock::time_point timestamp;
};

// ============================================================================
// 蓝牙监测器类
// ============================================================================
class BtMonitor {
public:
    BtMonitor();
    ~BtMonitor();

    // 初始化：连接系统总线上的 BlueZ 服务
    // 成功返回 true，无蓝牙适配器或 BlueZ 不可用时返回 false
    bool initialize();

    // 清理资源
    void cleanup();

    // 检查是否已初始化
    bool isInitialized() const { return initialized_.load(); }

    // 检查是否有蓝牙适配器可用
    bool hasAdapter() const;

    // ----- 适配器操作 -----

    // 获取适配器状态
    BtAdapterState getAdapterState() const;

    // 设置适配器电源 (开/关)
    bool setPowered(bool on);

    // 开启扫描 (发现设备)
    bool startDiscovery();

    // 停止扫描
    bool stopDiscovery();

    // ----- 设备查询 -----

    // 获取所有已知设备
    std::vector<BtDeviceInfo> getDevices() const;

    // 根据 MAC 获取设备信息
    bool getDevice(const std::string& mac, BtDeviceInfo* out) const;

    // 获取已连接设备列表
    std::vector<BtDeviceInfo> getConnectedDevices() const;

    // 获取附近设备 (最近 30 秒内见到)
    std::vector<BtDeviceInfo> getNearbyDevices(int maxAgeSec = 30) const;

    // ----- RSSI 查询 -----

    // 获取指定设备的当前 RSSI
    int16_t getDeviceRssi(const std::string& mac) const;

    // 获取所有设备的 RSSI 快照 (MAC → RSSI)
    std::map<std::string, int16_t> getRssiSnapshot() const;

    // ----- 事件获取 -----

    // 获取并清空待处理事件 (线程安全)
    std::vector<BtEvent> fetchEvents();

    // ----- 统计信息 -----

    size_t deviceCount() const;
    size_t connectedCount() const;

    // ----- 周期刷新 (由工作线程调用) -----

    // 执行一轮设备状态刷新
    void refreshDeviceStates();

    // 获取适配器属性 (使用内部 sysConn_)
    bool refreshAdapterState();

private:
    // 解析 BlueZ 返回的设备属性
    BtDeviceInfo parseDeviceProperties(DBusConnection* sysConn,
                                        const std::string& devPath);

    // 获取 BlueZ 适配器下的所有设备对象路径
    std::vector<std::string> listDevicePaths(DBusConnection* sysConn);

    // 通过 D-Bus 读取对象属性
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

    // 调用 BlueZ 方法 (无返回值)
    bool callBlueZMethod(const std::string& objPath,
                         const std::string& iface,
                         const std::string& method);

    // 发送 D-Bus 消息并获取回复
    DBusMessage* sendWithReply(DBusConnection* conn, DBusMessage* msg, int timeoutMs = 3000);

private:
    // 系统总线连接 (BlueZ 在系统总线上)
    DBusConnection* sysConn_ = nullptr;

    // 运行状态
    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};

    // 工作线程
    std::unique_ptr<std::thread> workerThread_;

    // 适配器状态
    mutable std::mutex adapterMutex_;
    BtAdapterState adapterState_;
    std::string adapterPath_;                    // BlueZ 对象路径，如 "/org/bluez/hci0"

    // 设备列表 (MAC → 设备信息)
    mutable std::mutex deviceMutex_;
    std::map<std::string, BtDeviceInfo> devices_;

    // 事件队列
    mutable std::mutex eventMutex_;
    std::vector<BtEvent> pendingEvents_;

    // 上次扫描开始时间
    std::chrono::system_clock::time_point lastDiscoveryStart_;
    static constexpr int DISCOVERY_TIMEOUT_SEC = 30;  // 扫描持续时间上限

    // 频率控制
    static constexpr int REFRESH_INTERVAL_SEC = 3;    // 设备刷新间隔
};

// ============================================================================
// 启动蓝牙监测线程 (在 server.cpp 中调用)
// ============================================================================
struct ServerContext;
void start_bt_monitor_thread(ServerContext* ctx);

}  // namespace weaknet_dbus
