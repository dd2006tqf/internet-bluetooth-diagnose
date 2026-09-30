/**
 * @file bt_event_monitor.hpp
 * @brief 蓝牙设备事件采集器（eBPF ringbuf -> RawBtObservation -> 归一化器）
 *
 * 数据流位置：
 *
 *   bt_events.bpf.c（内核 kprobe）
 *        ↓ ringbuf（原始观测，非业务事件）
 *   BtEventMonitor::consumeLoop()      ← 本类
 *        ↓ 转换为 RawBtObservation
 *   BtEventNormalizer::submit()
 *        ↓ 同一故障的多条观测在时间窗内合并
 *   WirelessDeviceEvent → WirelessEventStore
 *
 * 关键设计（与 plan 的语义约束一致）：
 *
 *   1. **本类不产生业务事件**。它把内核观测转成 RawBtObservation 投给归一化器，
 *      由归一化器决定"多个 hook 观测到一次断连"最终产生几个业务事件。
 *      若在这里各 hook 自产事件，同一物理故障会被重复计数。
 *
 *   2. **逐个 hook 独立挂载、独立降级**。某个 kprobe 在内核上不存在
 *      （例如不同内核版本缺少 hci_conn_timeout）时，只记日志并跳过，
 *      其余 hook 继续工作，绝不整体失败。
 *
 *   3. **不做诊断推断**。suspected_cause 恒为空，由后续 Phase 的
 *      LinkAnomalyDetector 产生。
 *
 *   4. **不做 CO-RE 读蓝牙结构**。目标内核蓝牙是模块，hci_conn/l2cap_chan
 *      不在 vmlinux BTF 中；结构偏移来自板端模块 BTF 实锤，写在 .bpf.c 注释里。
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "bt_event_normalizer.hpp"
#include "ebpf_monitor_interface.hpp"
#include "ebpf_monitor_metrics.hpp"
#include "wireless_event_store.hpp"

struct bpf_object;
struct bpf_link;
struct ring_buffer;

namespace weaknet_dbus {

class BtLinkQualityTracker;  ///< 前置声明：Phase 2 RSSI 回填源

/// 单个挂点的挂载结果（用于诊断日志与 health 上报）
struct BtHookStatus {
    std::string kernel_function;   ///< 内核函数名，如 "mgmt_device_disconnected"
    bool attached = false;         ///< 是否挂载成功
    std::string error;             ///< 失败原因（成功时为空）
};

/**
 * @brief 蓝牙设备事件采集器
 */
class BtEventMonitor : public IEbpfMonitor {
public:
    /**
     * @param store   事件存储（不拥有所有权；可为 nullptr，此时只归一化不落库，
     *                用于单元测试/降级）
     * @param tracker 链路质量跟踪器（不拥有所有权；用于 Canonical 事件定案后的因果 RSSI 回填）
     */
    explicit BtEventMonitor(WirelessEventStore* store = nullptr,
                            BtLinkQualityTracker* tracker = nullptr,
                            BtNormalizerConfig normalizer_cfg = {});
    ~BtEventMonitor() override;

    BtEventMonitor(const BtEventMonitor&) = delete;
    BtEventMonitor& operator=(const BtEventMonitor&) = delete;

    /**
     * @brief 加载 eBPF 对象并逐个挂载 kprobe
     *
     * 至少一个 hook 挂载成功即视为可用；全部失败则进入 Fallback
     * （用户空间不崩溃，isAvailable() 返回 false）。
     *
     * @param bpf_object_path bt_events.bpf.o 路径
     * @param gateway_id      网关身份，写入每条观测（事件来源身份）
     * @return true 至少一个 hook 挂载成功
     */
    bool init(const std::string& bpf_object_path, const std::string& gateway_id);

    /// 停止采集：停消费线程、detach 全部 hook、释放 BPF 对象
    void stop();

    /// 启动 ringbuf 消费线程（init 成功后调用）
    bool start();

    // ---- IEbpfMonitor 实现 ----
    const char* monitorName() const override { return "BtEventMonitor"; }
    EbpfMonitorState commonState() const override;
    bool isAvailable() const override;
    EbpfMonitorHealth health() const override { return stateSupport_.health(); }
    EbpfMonitorMetrics metrics() const override { return stateSupport_.metrics(); }
    void resetMetrics() override { stateSupport_.resetMetrics(); }

    // ---- 诊断 ----

    /// 全部挂点的挂载状态（含失败的及失败原因）
    std::vector<BtHookStatus> hookStatuses() const;

    /// 成功挂载的 hook 名列表，形如 "mgmt_device_disconnected,hci_disconnect"
    std::string attachedHooksSummary() const;

    /// 最近一次错误信息
    std::string lastError() const;

    /// 已从 ringbuf 消费的原始观测条数
    uint64_t observationsConsumed() const { return observations_consumed_.load(); }

    /// 已定案并写入 store 的 canonical 事件条数
    uint64_t eventsEmitted() const { return events_emitted_.load(); }

    /// 归一化后取走事件（供上层周期性调用；内部已按窗口刷出）
    std::vector<WirelessDeviceEvent> drainNormalizedEvents();

    /// 归一化器只读访问（诊断/测试）
    const BtEventNormalizer& normalizer() const { return normalizer_; }

private:
    /**
     * 内核上报结构 —— 必须与 server/src/bpf/bt_events.bpf.c 的
     * struct bt_observation 逐字段一致（含顺序与对齐）。
     * 两边任一改动都必须同步，否则 ringbuf 内容会被错误解释。
     *
     * 自然对齐布局：u64(0) u32(8) u8[6](12) u8(18..23) u8[32](24) = 56 字节。
     * bpf 侧不加 packed（packed 触发非对齐访问，部分 eBPF 校验器会拒绝），
     * 本侧同样不加，并以 static_assert 守住 56 字节契约。
     *
     * 字段语义（域区分是关键，见 wireless_event.hpp）：
     *   addr_type   — HCI 域 ADDR_LE_DEV_*（0=public,1=random,...），**不是** BDADDR_*
     *   link_type   — 内核 link_type（ACL_LINK=0x01 / LE_LINK=0x80），
     *                 与 addr_type 配合才能正确归类 BR/EDR vs LE Public vs LE Random
     *   reason      — 域取决于 source：mgmt 来源是 MGMT_DEV_DISCONN_* 枚举，
     *                 其余来源是 HCI error code；两者不能互套映射
     *   source_detail — 最长 hook 名 "mgmt_device_disconnected"(24 字符)+NUL=25，
     *                 因此数组 32 字节，小于 25 会截断（曾用 24 踩过）
     */
    struct KernelBtObservation {
        uint64_t timestamp_ns;
        uint32_t hci_index;
        uint8_t bdaddr[6];
        uint8_t addr_type;
        uint8_t event_type;
        uint8_t source;
        uint8_t has_reason;
        uint8_t reason;
        uint8_t link_type;
        uint8_t source_detail[32];
    };
    static_assert(sizeof(KernelBtObservation) == 56,
                  "KernelBtObservation 必须与 bt_events.bpf.c 对齐（56 字节）");

    /// ringbuf 回调入口（静态，转发到实例）
    static int onRingbufSample(void* ctx, void* data, size_t size);

    /// 处理单条内核观测
    void handleObservation(const KernelBtObservation& obs);

    /// 挂载一个 kprobe 程序
    BtHookStatus attachOne(const std::string& kernel_function,
                           const std::string& program_name);

    /// 消费线程主体：周期性 poll ringbuf 并刷出过期事件
    void consumeLoop();

    /// 把归一化器刷出的事件写入 store
    void flushToStore(uint64_t now_ms);

    /// 推进区域异常关联器的静默期（Phase 3a：让"没有新异常"也能推动 incident 结案）
    void advanceIncidents(uint64_t now_ms);

    WirelessEventStore* store_ = nullptr;              ///< 不拥有所有权
    BtLinkQualityTracker* tracker_ = nullptr;          ///< 不拥有所有权（因果 RSSI 回填源）
    BtEventNormalizer normalizer_;
    std::string gateway_id_;

    mutable std::mutex mutex_;
    bpf_object* bpf_obj_ = nullptr;
    ring_buffer* ringbuf_ = nullptr;
    std::vector<bpf_link*> links_;   ///< 与 hooks_ 一一对应的挂载句柄
    std::vector<BtHookStatus> hooks_;
    int cfg_map_fd_ = -1;

    std::thread consumer_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> observations_consumed_{0};
    std::atomic<uint64_t> events_emitted_{0};

    std::string bpf_object_path_;
    std::string last_error_;
    EbpfMonitorStateSupport stateSupport_{"BtEventMonitor"};
};

}  // namespace weaknet_dbus
