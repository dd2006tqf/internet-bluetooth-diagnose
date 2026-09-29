/**
 * @file bt_event_normalizer.hpp
 * @brief 蓝牙原始观测归一化器（RawBtObservation -> Canonical WirelessDeviceEvent）
 *
 * 本类解决一个核心问题：**事实来源有很多个，业务事件必须只有一个**。
 *
 * 数据流（严格单向，不可跳步）：
 *
 *   Kernel hooks (mgmt_device_disconnected / hci_conn_timeout / hci_disconnect / ...)
 *        ↓ 每个 hook 只投递一条 RawBtObservation，不产生任何业务事件
 *   BtEventNormalizer::submit(observation)
 *        ↓ 同一现实故障的多条观测在时间窗内合并
 *   WirelessDeviceEvent（Canonical Device Event）
 *        ↓
 *   WirelessEventStore::recordEvent()
 *        ↓
 *   device_events 表
 *
 * 归一化规则：
 *
 *   1. 合并键（不是简单按 MAC）：
 *        (gateway_id, hci_index, address_type, device_address, event_type)
 *      同一个 MAC 可能被不同 Controller / 不同网关观察到，那是两台探针
 *      各自独立看到的事实，不应该被合并。
 *
 *   2. 合并窗口：kMergeWindowMs（默认 2000ms）。窗口内同一键的观测归入
 *      同一个 canonical 事件；窗口外视为另一次真实事件，绝不合并。
 *
 *   3. 事件时间取窗口中**最早**一条观测的时刻——现实故障发生在最早
 *      观测到的那一刻，而不是最后一条证据到达的时刻。
 *
 *   4. 原因定案优先级（不是"谁先到用谁"）：
 *        mgmt reason（内核 mgmt 层给出的原始 code）
 *            > supplemental hook 推断（如 hci_conn_timeout -> ConnectionTimeout）
 *            > Unknown
 *      所有来源一律存进 raw_evidence，优先级只决定哪条成为 canonical reason，
 *      不会覆盖或丢弃任何原始事实。
 *
 *   5. 本类不产出 suspected_cause —— 它是归一化器，不是诊断器。
 *
 * 线程安全：所有方法由内部 mutex 保护，可跨线程调用。
 */

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <mutex>
#include <string>
#include <vector>

#include "wireless_event.hpp"

namespace weaknet_dbus {

/**
 * @brief 归一化配置
 */
struct BtNormalizerConfig {
    /// 合并窗口（毫秒）。窗口内同一合并键的观测归入同一 canonical 事件。
    uint64_t merge_window_ms = 2000;
    /// 待定事件上限：超过后强制刷出最老的一条（防止内存无界增长）
    size_t max_pending = 512;
    /// 已定案事件的 ID 前缀（真实 event_id 由调用方在入库时补齐，
    /// 这里只保证同一批内唯一、可读）
    std::string event_id_prefix = "btev";
};

/**
 * @brief 蓝牙原始观测归一化器
 *
 * 用法：
 *   BtEventNormalizer normalizer(cfg);
 *   //  一次 timeout 断连的两条观测：
 *   normalizer.submit(timeout_obs);      // hci_conn_timeout
 *   normalizer.submit(mgmt_obs);         // mgmt_device_disconnected, reason=0x08
 *   //  窗口结束后：
 *   auto events = normalizer.flushExpired(now_ms);  // -> 1 个 LinkDisconnected
 *   // 该事件的 details_json.raw_evidence 含 2 条记录，
 *   // reason = ConnectionTimeout，raw_reason_code = 0x08
 */
class BtEventNormalizer {
public:
    explicit BtEventNormalizer(BtNormalizerConfig cfg = {});

    /**
     * @brief 投递一条原始观测
     *
     * 若该观测的合并键在合并窗口内已有 pending 事件，则归入该事件
     * （更新原因定案、追加 raw_evidence）；否则创建新的 pending 事件。
     *
     * @param obs 原始观测
     */
    void submit(const RawBtObservation& obs);

    /**
     * @brief 刷出所有已超合并窗口的 pending 事件
     *
     * @param now_ms 当前时刻（墙钟毫秒）
     * @return 本批定案的 canonical 事件（已附 event_id，但不写库）
     *
     * 调用方负责把返回的事件交给 WirelessEventStore::recordEvent()。
     * 建议调用节奏：消费线程每 500ms 调用一次。
     */
    std::vector<WirelessDeviceEvent> flushExpired(uint64_t now_ms);

    /**
     * @brief 无条件定案全部 pending 事件（进程退出、测试用）
     */
    std::vector<WirelessDeviceEvent> flushAll();

    /// 当前 pending 事件数（诊断/测试用）
    size_t pendingCount() const;

    /// 自上次调用以来定案的 canonical 事件累计数（诊断/测试用）
    uint64_t normalizedCount() const;

    /// 重置全部状态（适配器重启、D-Bus 重连等场景）
    void reset();

    const BtNormalizerConfig& config() const { return cfg_; }

private:
    /**
     * @brief 合并键
     *
     * 覆盖"同一物理故障"的识别边界：同网关、同控制器、同地址类型、同设备、
     * 同事件类型。任一不同即视为不同事实（例如同 MAC 在两台网关各自被观测）。
     */
    struct MergeKey {
        std::string gateway_id;
        uint32_t hci_index = 0;
        BtAddressType address_type = BtAddressType::Unknown;
        std::string device_address;
        DeviceEventType event_type = DeviceEventType::LinkDisconnected;

        bool operator<(const MergeKey& other) const;
    };

    /// 一条正在窗口内累积的事件
    struct PendingEvent {
        WirelessDeviceEvent event;          ///< 已基本成形的 canonical 事件
        std::vector<RawBtObservation> evidence;  ///< 全部原始观测
        uint64_t last_obs_ms = 0;           ///< 最新一条观测时刻（用于合并窗口判定）
        int reason_rank = -1;               ///< 当前定案原因的优先级（见 outranksCurrent）
    };

    static MergeKey makeKey(const RawBtObservation& obs);

    /// 把一条观测应用到 pending 事件上（更新原因定案、追加证据、合并 RSSI）
    void applyObservation(PendingEvent& pending, const RawBtObservation& obs);

    /// 定案：把 PendingEvent 转为最终的 WirelessDeviceEvent（补 event_id、
    /// 拼 details_json.raw_evidence、确认 reason/source/source_detail）
    WirelessDeviceEvent finalize(PendingEvent& pending, uint64_t seq);

    /**
     * @brief 原因定案优先级（规则 4）
     *
     * 返回值是该观测携带原因的优先级分数（越大越优先）：
     *
     *   3 = mgmt 层的原始码（MGMT_DEV_DISCONN_*，最权威的事实）
     *   2 = 其余来源携带的原始码（如 hci_disconnect 的 HCI error code——
     *       原始码是事实，必须排在 hint 之上，否则先到的 raw 会被
     *       后到的推断 hint 覆盖）
     *   1 = supplemental hook 的 reason_hint（如 hci_conn_timeout -> ConnectionTimeout）
     *   0 = 没有带来任何原因信息
     *  -1 = 定案前的初始值
     *
     * 注意：优先级只决定哪条成为 canonical 字段，obs 本身始终进入 raw_evidence，
     * 因此原始事实不会因为优先级比较而丢失。
     */
    static int reasonRank(const RawBtObservation& obs);

    BtNormalizerConfig cfg_;
    /// 实例随机短码（构造时生成）。event_id = <prefix>_<nonce>_<seq>，
    /// **必须带 nonce**：seq 每次进程启动都从 0 开始，而 device_events.event_id
    /// 有 UNIQUE 约束且落库用 INSERT OR IGNORE —— 服务重启后第一批事件的
    /// btev_0 会与历史行冲突并被**静默丢弃**（每次部署都会重启服务，必然踩中）。
    /// 注释里曾写"全局唯一性由 Store 层负责"，但 Store 并未做这件事——
    /// 唯一性必须由生成方自己保证。
    std::string instance_nonce_;
    std::map<MergeKey, PendingEvent> pending_;
    /// 已定案但尚未被 flushExpired/flushAll 取走的事件
    std::vector<WirelessDeviceEvent> completed_;
    uint64_t seq_ = 0;             ///< 事件序号，用于 event_id 后缀
    uint64_t normalized_total_ = 0;
    mutable std::mutex mu_;
};

}  // namespace weaknet_dbus
