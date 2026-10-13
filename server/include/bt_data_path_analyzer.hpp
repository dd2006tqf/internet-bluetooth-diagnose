#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "bt_hci_connection_tracker.hpp"
#include "ebpf_monitor_interface.hpp"
#include "ebpf_monitor_metrics.hpp"

// 前置声明 libbpf 类型
struct bpf_object;
struct bpf_link;

namespace weaknet_dbus {

/**
 * @brief L1 蓝牙 HCI ACL 纯净数据通路观测 DTO
 * 严格只保存客观事实与计数，绝不包含任何推断与假阳性故障判定。
 */
struct BtDataPathObservation {
    std::string observation_type{"BT_HCI_ACL_TRAFFIC"};
    std::string controller_id{"hci0"};
    uint16_t connection_handle{0};
    uint32_t connection_generation{0};
    std::string attribution_scope{"unknown"}; // "device" | "controller" | "unknown"
    std::optional<std::string> device_id{std::nullopt}; // 设备规范 MAC（若已知）

    uint32_t window_ms{0};
    uint64_t tx_bytes{0};       // 窗口内增量 ACL payload 字节
    uint64_t rx_bytes{0};       // 窗口内增量 ACL payload 字节
    uint64_t tx_packets{0};     // 窗口内增量 ACL 报文数
    uint64_t rx_packets{0};     // 窗口内增量 ACL 报文数

    double tx_throughput_bps{0.0}; // 窗口内平均速率 bits/s
    double rx_throughput_bps{0.0}; // 窗口内平均速率 bits/s

    uint32_t max_gap_ms{0};     // 窗口内相邻收发最大时间跨度（毫秒）
    std::string status{"OBSERVED"};

    // 原始单调时间戳（纳秒）
    uint64_t last_tx_ktime_ns{0};
    uint64_t last_rx_ktime_ns{0};

    std::string toJson() const;
};

/**
 * @brief 内核 BPF Map 中的原始累计值镜像
 */
struct RawAclTrafficVal {
    uint64_t tx_packets{0};
    uint64_t tx_bytes{0};
    uint64_t rx_packets{0};
    uint64_t rx_bytes{0};
    uint64_t last_tx_ktime_ns{0};
    uint64_t last_rx_ktime_ns{0};
    uint64_t non_acl_dropped{0};
    uint64_t short_pkt_dropped{0};
};

/**
 * @brief 工业蓝牙 HCI ACL 数据通道流量分析器
 * 负责加载 bt_hci_traffic.bpf.o，周期性扫描 Map 并结合 BtHciConnectionTracker
 * 产出具备代次隔离和真实设备/控制器归因的 BtDataPathObservation。
 */
class BtDataPathAnalyzer : public IEbpfMonitor {
public:
    explicit BtDataPathAnalyzer(std::shared_ptr<weaknet::BtHciConnectionTracker> tracker = nullptr);
    ~BtDataPathAnalyzer() override;

    BtDataPathAnalyzer(const BtDataPathAnalyzer&) = delete;
    BtDataPathAnalyzer& operator=(const BtDataPathAnalyzer&) = delete;

    /**
     * @brief 初始化并加载 eBPF 对象
     * @param bpfObjectPath 编译好的 bt_hci_traffic.bpf.o 路径
     * @param controller 默认控制器名称（如 "hci0"）
     * @return true 挂载成功；false 降级为离线模式
     */
    bool init(const std::string& bpfObjectPath, const std::string& controller = "hci0");

    /// 停止分析器：释放挂点与 BPF 对象
    void stop();

    // ---- IEbpfMonitor 统一接口 ----
    const char* monitorName() const override { return "BtDataPathAnalyzer"; }
    EbpfMonitorState commonState() const override;
    bool isAvailable() const override;
    EbpfMonitorHealth health() const override { return stateSupport_.health(); }
    EbpfMonitorMetrics metrics() const override { return stateSupport_.metrics(); }
    void resetMetrics() override { stateSupport_.resetMetrics(); }

    std::string attachedHookName() const;
    std::string lastError() const;

    /**
     * @brief 周期性采样并计算当前时间窗口内的观测增量
     * @param now_point 当前参考时间点（若默认则采用 steady_clock::now()）
     * @return 活跃/有流量连接的观测列表
     */
    std::vector<BtDataPathObservation> collectObservations(
        std::chrono::steady_clock::time_point now_point = std::chrono::steady_clock::now());

    /**
     * @brief 直接从内核 Map 读取单连接累计原始值（测试与诊断）
     */
    bool readRawMapValue(uint16_t handle, RawAclTrafficVal* out) const;

    /**
     * @brief 手动注入原始累计数据（无内核环境下的单元测试与算法验证桩）
     */
    void injectMockSample(uint16_t handle, const RawAclTrafficVal& val);

    /**
     * @brief 删除不再对应活跃内核连接的 Map 条目（含内存测试桩）
     *
     * 内核断连后 BPF Map 条目不会自动消失：长期运行会把 64 条 HASH 槽位
     * 耗尽在死句柄上（新连接 BPF_NOEXIST 建项失败 → 流量漏计），且死句柄的
     * 残留累计值会持续产出"controller"归因的幻影观测。调用方以内核连接表
     * （Tracker 活跃集）为真值，删除不在活跃集的条目。
     *
     * @param live_handles 当前内核活跃句柄集合（12 位）
     * @return 删除的条目数
     */
    size_t pruneStaleEntries(const std::set<uint16_t>& live_handles);

    std::shared_ptr<weaknet::BtHciConnectionTracker> connectionTracker() const {
        return tracker_;
    }

private:
    struct LastSample {
        RawAclTrafficVal val;
        std::chrono::steady_clock::time_point time;
        uint32_t generation{0};
    };

    mutable std::mutex mutex_;
    std::shared_ptr<weaknet::BtHciConnectionTracker> tracker_;
    std::string controller_id_{"hci0"};
    std::string bpfObjectPath_;

    bpf_object* bpfObj_ = nullptr;
    bpf_link* link_tx_ = nullptr;
    bpf_link* link_rx_ = nullptr;
    int mapFd_ = -1;
    int globalMapFd_ = -1;

    EbpfMonitorState state_{EbpfMonitorState::Uninitialized};
    std::string lastError_;
    std::string attachedHookName_;
    EbpfMonitorStateSupport stateSupport_{"BtDataPathAnalyzer"};

    // 历史采样快照缓存：key = handle (12位) -> LastSample
    std::unordered_map<uint16_t, LastSample> last_samples_;

    // 内存测试桩数据（当 mapFd_ < 0 时生效）
    std::unordered_map<uint16_t, RawAclTrafficVal> mock_map_;
};

} // namespace weaknet_dbus
