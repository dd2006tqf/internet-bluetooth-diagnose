/**
 * @file bt_audio_analyzer.hpp
 * @brief 蓝牙音频 eBPF 分析器 — 加载、管理 eBPF 程序并读取内核态流量统计
 *
 * 设计要点：
 *   - 挂点自动探测：按优先级尝试 kprobe/l2cap_sock_sendmsg → kprobe/l2cap_chan_send
 *     任一挂点成功即标记为可用；全部失败则标记为不可用，用户空间降级回纯 D-Bus
 *   - 会话控制：setSessionActive(mac, active) 写入 BPF map 控制内核态跟踪开关
 *     只有被启用的设备才会被内核态程序统计
 *   - 数据读取：getStats() / getAllStats() 从内核态累计 map 读取，
 *     用户空间负责增量计算（与前次快照做 diff）
 *   - 线程安全：所有公开方法内部加锁（mutex_ 保护 map fd / 状态 / 统计快照）
 *   - 指标聚合：继承 EbpfMonitorStateSupport，自动获得 health() / metrics() / resetMetrics()
 *
 * 内核态程序：server/src/bpf/a2dp_media.bpf.c
 *   挂点：kprobe/l2cap_sock_sendmsg（首选）或 kprobe/l2cap_chan_send（备选）
 *   Maps：bt_traffic（累计统计）、active_sessions（跟踪开关）、btaudio_cfg（阈值配置）
 */

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
// eBPF 采集的蓝牙流量统计（对应内核态 bt_traffic map 的值结构）
// ============================================================================
struct BtTrafficStats {
    uint64_t bytes = 0;          ///< 窗口内累计字节数（内核态累加）
    uint64_t packets = 0;        ///< 窗口内累计包数
    uint64_t lastPacketNs = 0;   ///< 最后一包时间戳 (CLOCK_MONOTONIC ns)
    uint64_t gapCount = 0;       ///< 间隔 > 100ms 的次数（内核态计算）
    uint64_t maxGapNs = 0;       ///< 最大包间隔（纳秒），用于卡顿检测
};

// ============================================================================
// eBPF 分析器状态
// ============================================================================
enum class BtAudioAnalyzerState {
    Uninitialized,   ///< 尚未初始化
    Attached,        ///< eBPF 程序已挂载，正常运行
    Fallback,        ///< 挂载失败，已降级为离线模式（用户空间不崩溃）
    Error,           ///< 初始化过程中发生错误
    Stopped,         ///< 已显式 stop()
};

// ============================================================================
// 蓝牙音频 eBPF 分析器
//
// 继承 IEbpfMonitor，提供统一的健康/指标查询接口。
// 同时扩展了会话控制、统计读取等蓝牙特有的功能。
// ============================================================================
class BtAudioAnalyzer : public IEbpfMonitor {
public:
    BtAudioAnalyzer();
    ~BtAudioAnalyzer();

    // ---- 禁止拷贝 ----
    BtAudioAnalyzer(const BtAudioAnalyzer&) = delete;
    BtAudioAnalyzer& operator=(const BtAudioAnalyzer&) = delete;

    // ---- 生命周期 ----

    /**
     * @brief 初始化 eBPF 分析器
     *
     * 加载 BPF 对象 → 尝试挂载 kprobe → 记录 map fd。
     * 任一挂点成功即标记可用；全部失败则标记为 Fallback。
     *
     * @param bpfObjectPath BPF 目标文件路径（如 "build/a2dp_media.bpf.o"）
     * @return true  至少一个挂点成功
     * @return false 全部失败（用户空间不崩溃，后续 isAvailable() 返回 false）
     */
    bool init(const std::string& bpfObjectPath);

    /// 停止分析器：卸载 eBPF 程序，关闭 BPF 对象
    void stop();

    /// 当前内部状态（BtAudioAnalyzerState，比 EbpfMonitorState 更细粒度）
    BtAudioAnalyzerState state() const;

    bool isAvailable() const override;

    // ---- IEbpfMonitor 实现 ----
    const char* monitorName() const override { return "BtAudioAnalyzer"; }
    EbpfMonitorState commonState() const override;
    EbpfMonitorHealth health() const override { return stateSupport_.health(); }
    EbpfMonitorMetrics metrics() const override { return stateSupport_.metrics(); }
    void resetMetrics() override { stateSupport_.resetMetrics(); }

    /// 获取挂载成功的钩子名称（用于日志报告，如 "kprobe/l2cap_sock_sendmsg"）
    std::string attachedHookName() const;

    // ---- 会话控制 ----

    /**
     * @brief 设置指定蓝牙设备的跟踪状态
     *
     * 写入 active_sessions map；内核态程序只对被启用的设备统计流量。
     *
     * @param mac       设备 MAC 地址（格式 "AA:BB:CC:DD:EE:FF"）
     * @param active    true=启用跟踪, false=停止跟踪
     * @param direction 方向：0=发送, 1=接收（默认跟踪发送）
     * @return true 若操作成功（BPF map 写入成功）
     */
    bool setSessionActive(const std::string& mac, bool active, uint8_t direction = 0);

    /// 全局启用/禁用 eBPF 跟踪（写入 btaudio_cfg map）
    void setGlobalEnabled(bool enabled);

    // ---- 数据读取 ----

    /**
     * @brief 获取指定设备的流量统计（累计值，调用方负责增量计算）
     * @param mac       设备 MAC 地址
     * @param direction 方向：0=发送, 1=接收
     * @param out       输出参数，接收统计值
     * @return true 若找到该设备的统计记录
     */
    bool getStats(const std::string& mac, uint8_t direction, BtTrafficStats* out) const;

    /**
     * @brief 获取所有活跃设备的流量统计快照
     * @return MAC → 统计的向量（方向已编码在 MAC 字符串或由调用方分别查询）
     */
    std::vector<std::pair<std::string, BtTrafficStats>> getAllStats() const;

    // ---- 管理接口 ----

    /// 清理指定设备的统计记录（通常在设备断开时调用，避免 map 泄漏）
    void clearDeviceStats(const std::string& mac);

    /// 获取挂载过程中发生的错误信息（用于诊断报告）
    std::string lastError() const;

private:
    // ---- 内部辅助 ----

    /**
     * @brief 解析 MAC 地址字符串为 6 字节数组
     * @param mac     输入 MAC（"AA:BB:CC:DD:EE:FF" 格式）
     * @param bdaddr  输出 6 字节数组
     * @return true 解析成功
     */
    static bool parseMac(const std::string& mac, uint8_t bdaddr[6]);

    /// 构造 device_key（MAC + 方向），用于 BPF map 查找
    static void fillDeviceKey(const std::string& mac, uint8_t direction, uint8_t key_bdaddr[6], uint8_t& key_dir);

    /**
     * @brief 尝试挂载单个 kprobe 程序
     * @param funcName  内核函数名（如 "l2cap_sock_sendmsg"）
     * @param progName  BPF 对象内的程序名（如 "trace_l2cap_send"）
     * @return 成功时返回 bpf_link 指针，失败返回 nullptr
     */
    bpf_link* tryAttachKprobe(const std::string& funcName, const std::string& progName);

    /// 从 BPF map 读取统计值（bt_traffic map key = MAC + direction）
    bool readStatsFromMap(const uint8_t bdaddr[6], uint8_t direction, BtTrafficStats* out) const;

    // ---- 成员变量 ----

    mutable std::mutex mutex_;           ///< 保护所有 libbpf 句柄和状态字段

    bpf_object* bpfObj_ = nullptr;       ///< libbpf BPF 对象句柄（init 加载，stop 释放）
    bpf_link* link1_ = nullptr;          ///< 挂点 1 的链接句柄（首选 kprobe）
    bpf_link* link2_ = nullptr;          ///< 挂点 2 的链接句柄（备选 kprobe）

    int statsMapFd_ = -1;                ///< bt_traffic map 的 fd
    int sessionsMapFd_ = -1;             ///< active_sessions map 的 fd
    int cfgMapFd_ = -1;                  ///< btaudio_cfg map 的 fd（全局开关 + 阈值）

    BtAudioAnalyzerState state_ = BtAudioAnalyzerState::Uninitialized;
    std::string attachedHookName_;       ///< 成功挂载的钩子名称（用于日志）
    std::string lastError_;              ///< 最近的错误信息（诊断用）
    std::string bpfObjectPath_;          ///< 当前 BPF 对象路径
    EbpfMonitorStateSupport stateSupport_{"BtAudioAnalyzer"};  ///< 指标追踪/健康查询
};

}  // namespace weaknet_dbus
