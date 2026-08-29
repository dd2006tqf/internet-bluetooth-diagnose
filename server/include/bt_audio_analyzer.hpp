// bt_audio_analyzer.hpp
// 蓝牙音频 eBPF 分析器 — 加载、管理 eBPF 程序并读取内核态流量统计
//
// 设计要点：
//   - 挂点自动探测：按优先级尝试 kprobe/l2cap_sock_sendmsg → kprobe/l2cap_chan_send
//   - 任一挂点成功即标记为可用；全部失败则标记为不可用，用户空间降级回纯 D-Bus
//   - 提供 setSessionActive() 控制 eBPF 跟踪开关
//   - 提供 getStats() 读取内核态累计统计，用户空间负责增量计算
//   - 线程安全：所有公开方法内部加锁

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <cstdint>
#include <cstddef>
#include "ebpf_monitor_interface.hpp"
#include "ebpf_monitor_metrics.hpp"

// 前置声明 libbpf 类型
struct bpf_object;
struct bpf_link;
struct bpf_map;

namespace weaknet_dbus {

// ============================================================================
// eBPF 采集的蓝牙流量统计
// ============================================================================
struct BtTrafficStats {
    uint64_t bytes = 0;          // 窗口内累计字节数
    uint64_t packets = 0;        // 窗口内累计包数
    uint64_t lastPacketNs = 0;   // 最后一包时间戳 (CLOCK_MONOTONIC ns)
    uint64_t gapCount = 0;       // 间隔 > 100ms 的次数
    uint64_t maxGapNs = 0;       // 最大包间隔 (纳秒)
};

// ============================================================================
// eBPF 分析器状态
// ============================================================================
enum class BtAudioAnalyzerState {
    Uninitialized,   // 尚未初始化
    Attached,        // eBPF 程序已挂载，正常运行
    Fallback,        // 挂载失败，已降级为离线模式
    Error,           // 初始化过程中发生错误
    Stopped,         // 已停止
};

// ============================================================================
// 蓝牙音频 eBPF 分析器
// ============================================================================
class BtAudioAnalyzer : public IEbpfMonitor {
public:
    BtAudioAnalyzer();
    ~BtAudioAnalyzer();

    // ---- 禁止拷贝 ----
    BtAudioAnalyzer(const BtAudioAnalyzer&) = delete;
    BtAudioAnalyzer& operator=(const BtAudioAnalyzer&) = delete;

    // ---- 生命周期 ----

    // 初始化 eBPF 分析器：加载 BPF 对象，探测并挂载程序
    // @param bpfObjectPath BPF 目标文件路径（如 "build/a2dp_media.bpf.o"）
    // @return true 若初始化成功（至少一个挂点成功），false 若全部失败
    bool init(const std::string& bpfObjectPath);

    // 停止分析器：卸载 eBPF 程序，关闭 BPF 对象
    void stop();

    // 获取当前状态
    BtAudioAnalyzerState state() const;

    // 是否可用（即 eBPF 程序已成功挂载）
    bool isAvailable() const override;

    const char* monitorName() const override { return "BtAudioAnalyzer"; }
    EbpfMonitorState commonState() const override;
    EbpfMonitorHealth health() const override { return stateSupport_.health(); }
    EbpfMonitorMetrics metrics() const override { return stateSupport_.metrics(); }
    void resetMetrics() override { stateSupport_.resetMetrics(); }

    // 获取挂载成功的钩子名称（用于日志报告）
    std::string attachedHookName() const;

    // ---- 会话控制 ----

    // 设置指定蓝牙设备的跟踪状态
    // @param mac 设备 MAC 地址（格式 "AA:BB:CC:DD:EE:FF"）
    // @param active true=启用跟踪, false=停止跟踪
    // @param direction 方向：0=发送, 1=接收（默认跟踪发送）
    // @return true 若操作成功
    bool setSessionActive(const std::string& mac, bool active, uint8_t direction = 0);

    // 全局启用/禁用 eBPF 跟踪
    void setGlobalEnabled(bool enabled);

    // ---- 数据读取 ----

    // 获取指定设备的流量统计（累计值，调用方负责增量计算）
    // @param mac 设备 MAC 地址
    // @param direction 方向：0=发送, 1=接收
    // @param out 输出参数，接收统计值
    // @return true 若找到该设备的统计记录
    bool getStats(const std::string& mac, uint8_t direction, BtTrafficStats* out) const;

    // 获取所有活跃设备的流量统计快照
    // @return MAC → (方向) → 统计的映射
    std::vector<std::pair<std::string, BtTrafficStats>> getAllStats() const;

    // ---- 管理接口 ----

    // 清理指定设备的统计记录（通常在设备断开时调用）
    void clearDeviceStats(const std::string& mac);

    // 获取挂载过程中发生的错误信息
    std::string lastError() const;

private:
    // ---- 内部方法 ----

    // 解析 MAC 地址字符串为 6 字节数组
    // @return true 若解析成功
    static bool parseMac(const std::string& mac, uint8_t bdaddr[6]);

    // 构造 device_key（用于 BPF map 查找）
    static void fillDeviceKey(const std::string& mac, uint8_t direction, uint8_t key_bdaddr[6], uint8_t& key_dir);

    // 尝试挂载单个 kprobe 程序
    // @return 成功时返回 bpf_link 指针，失败返回 nullptr
    bpf_link* tryAttachKprobe(const std::string& funcName, const std::string& progName);

    // 从 BPF map 读取统计值
    bool readStatsFromMap(const uint8_t bdaddr[6], uint8_t direction, BtTrafficStats* out) const;

    // ---- 成员变量 ----

    mutable std::mutex mutex_;           // 线程安全锁

    bpf_object* bpfObj_ = nullptr;       // libbpf BPF 对象句柄
    bpf_link* link1_ = nullptr;          // 挂点 1 的链接句柄
    bpf_link* link2_ = nullptr;          // 挂点 2 的链接句柄

    int statsMapFd_ = -1;                // bt_traffic map 的 fd
    int sessionsMapFd_ = -1;             // active_sessions map 的 fd
    int cfgMapFd_ = -1;                  // btaudio_cfg map 的 fd

    BtAudioAnalyzerState state_ = BtAudioAnalyzerState::Uninitialized;
    std::string attachedHookName_;       // 成功挂载的钩子名称
    std::string lastError_;              // 最近的错误信息，用于诊断
    std::string bpfObjectPath_;          // 当前 BPF 对象路径
    EbpfMonitorStateSupport stateSupport_{"BtAudioAnalyzer"};
};

}  // namespace weaknet_dbus