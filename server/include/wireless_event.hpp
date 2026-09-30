/**
 * @file wireless_event.hpp
 * @brief 无线设备事件领域模型（Phase 1：Bluetooth Device Event）
 *
 * 本文件定义"工业现场无线信号检测与诊断网关"的最小事件模型：
 *
 *   RawBtObservation  —— 内核探针采到的一条原始观测（事实碎片）
 *        ↓ BtEventNormalizer
 *   WirelessDeviceEvent —— 规范化业务事件（Canonical Device Event）
 *
 * 设计原则（五条硬约束，修改前必须读懂）：
 *
 *   1. WHAT 与 WHY 分离
 *      DeviceEventType 回答"发生了什么"，DisconnectReason 回答"为什么"。
 *      内核 hook 不得各自产出一个业务事件——`hci_conn_timeout` 不是一种事件，
 *      它是 `LinkDisconnected` 的一条证据。否则同一现实故障被多个 hook 各记一次，
 *      后续按事件计数（Phase 2/3 的掉线率、多设备关联）会翻倍。
 *
 *   2. 原始事实无损保存
 *      `raw_reason_code` 与归一化的 `reason` 同时保存。前者永不因后者而丢弃——
 *      否则以后想把 Other 细分时，历史数据已经无法回溯。
 *
 *   3. 事实与推断永不合并
 *      `reason` 是事实（来自内核），`suspected_cause` 是推断（来自诊断器）。
 *      本 Phase 没有诊断器，因此 suspected_cause 恒为 nullopt——
 *      采集层不承担诊断职责，填它的是 Phase 2/3 的 LinkAnomalyDetector。
 *
 *   4. 未采集 != 零值
 *      `rssi_at_event_dbm` 用 optional（DB 为 NULL）。用 0 冒充"没采到"会污染
 *      后续的 RSSI 统计与均值计算。
 *
 *   5. details_json 只放审计性证据，不放查询关键数据
 *      需要过滤/聚合的字段一律升为正式列。details_json 是逃生舱不是垃圾场。
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace weaknet_dbus {

// ============================================================================
// 枚举定义
// ============================================================================

/**
 * @brief 无线协议族
 *
 * Phase 1 只实例化 Bluetooth。其余值为 schema 预留——它们的**存在不代表已支持**，
 * 真正启动 Wi-Fi 设备模型时需要单独定义 WifiStationIdentity（MAC 随机化、
 * 漫游、STA/AP/BSSID 关联关系等都还没有语义）。
 */
enum class WirelessProtocol : uint8_t {
    Bluetooth = 0,
    Wifi      = 1,   ///< reserved，Phase 1 不产生此类事件
    Zigbee    = 2,   ///< reserved
    Other     = 3
};

/**
 * @brief 蓝牙设备地址类型（取值对齐 kernel mgmt 层 ABI）
 *
 * 0x00 = BR/EDR（经典蓝牙，无 LE 地址类型概念）
 * 0x01 = LE Public
 * 0x02 = LE Random
 *
 * 该取值空间是内核 ABI 定义的闭集，因此直接以 enum 值保存即为无损。
 * 0xFF 表示"本次观测未携带地址类型"（区别于 BR/EDR 的 0x00）。
 */
enum class BtAddressType : uint8_t {
    Bredr     = 0x00,
    LePublic  = 0x01,
    LeRandom  = 0x02,
    Unknown   = 0xFF
};

/**
 * @brief 设备事件类型 —— 回答 WHAT
 *
 * 注意：这里**没有** ConnectionTimeout / AuthFailed。
 * 那些是断连的原因（WHY），属于 DisconnectReason 的取值域，
 * 而不是独立的事件类型。详见文件头设计原则 1。
 */
enum class DeviceEventType : uint8_t {
    DeviceAppeared    = 0,   ///< 设备被发现（D-Bus 设备发现路径）
    DeviceLost        = 1,   ///< 设备消失
    LinkConnected     = 2,   ///< 链路已建立成功（需要可靠的成功来源，Phase 1 不产出）
    LinkDisconnected  = 3,   ///< 链路断开（Phase 1 的交付目标）
    LinkDegraded      = 4,   ///< 链路质量劣化（Phase 2 的 RSSI 基线检测产出）
    LinkRecovered     = 5,   ///< 链路恢复
    ConnectionAttempt = 6    ///< 连接尝试（"开始建链" != "已连上"，仅作底层观测）
};

/**
 * @brief 断连原因（归一化解释）—— 回答 WHY
 *
 * 由内核 HCI reason code 映射而来，取值范围刻意收窄以便产品展示。
 * 原始 code 始终同时保存在 WirelessDeviceEvent::raw_reason_code 中。
 */
enum class DisconnectReason : uint8_t {
    Unknown              = 0,
    RemoteUserTerminated = 1,   ///< 对端设备主动断开 (HCI 0x13/0x14/0x15)
    ConnectionTimeout    = 2,   ///< 链路超时 (HCI 0x08/0x10/0x22)
    AuthenticationFailure= 3,   ///< 配对/认证失败 (HCI 0x05/0x06/0x25/0x2F)
    LocalHostTerminated  = 4,   ///< 本机主动断开 (HCI 0x16)
    Other                = 5    ///< 已确认是断连，但原因不在上述分类内
};

/**
 * @brief 证据来源 —— 回答"这条结论是谁观测到的"
 *
 * 可追溯性是工业诊断的基本要求：数据库里一条 reason=ConnectionTimeout，
 * 必须能回答它来自 HCI reason、某个 kprobe，还是未来的某个算法推断。
 */
enum class EvidenceSource : uint8_t {
    Unknown          = 0,
    KernelMgmt       = 1,   ///< 内核 mgmt 层（bluetoothd 的统一消费出口）
    KernelHci        = 2,   ///< 内核 HCI 层（hci_disconnect / hci_conn_del）
    KernelHciTimeout = 3,   ///< 内核 HCI 链路超时（hci_conn_timeout）
    BluezDbus        = 4,   ///< 预留：BlueZ D-Bus 观测
    Derived          = 5    ///< 预留：诊断器推断
};

// ============================================================================
// 原始观测（Normalizer 的输入）
// ============================================================================

/**
 * @brief 一条未经归一化的内核原始观测
 *
 * 每个 kprobe 命中的效果是"往归一化器投递一条 observation"，
 * 而**不是**直接产生业务事件。归一化器负责把同一现实故障的多条观测
 * 合并成一个 WirelessDeviceEvent，并把其余观测存为该事件的 raw_evidence。
 *
 * 这样做的目的：事实来源可以有很多个，业务事件必须有且只有一个。
 */
struct RawBtObservation {
    uint64_t timestamp_ms = 0;                              ///< 观测时刻（墙钟毫秒）
    std::string gateway_id;                                 ///< 观测来源网关身份
    uint32_t hci_index = 0;                                 ///< 观测来源 HCI 适配器序号
    std::string device_address;                             ///< BDADDR，格式 XX:XX:XX:XX:XX:XX
    BtAddressType address_type = BtAddressType::Unknown;    ///< 内核 mgmt addr_type

    DeviceEventType event_type = DeviceEventType::LinkDisconnected;  ///< 观测到的既成事实

    EvidenceSource source = EvidenceSource::Unknown;        ///< 证据来源类别
    std::string source_detail;                              ///< 精确 hook 名，如 "mgmt_device_disconnected"

    /// 内核原始 reason code（未映射）。仅当该观测携带 reason 时有值。
    std::optional<uint8_t> raw_reason_code;
    /// 该来源直接给出的归一化原因（如 hci_conn_timeout 直接给出 ConnectionTimeout）
    DisconnectReason reason_hint = DisconnectReason::Unknown;

    std::optional<int> rssi_dbm;                            ///< 采集时刻的 RSSI（未采集则 nullopt）
    uint64_t monotonic_ns = 0;                              ///< 单调时钟时间戳（CLOCK_MONOTONIC / bpf_ktime_get_ns）
};

// ============================================================================
// 规范化业务事件（Canonical Device Event）
// ============================================================================

/**
 * @brief 规范化无线设备事件
 *
 * 一次真实的物理断连，无论内核中有多少个观测来源，最终只留下一个本结构实例。
 */
struct WirelessDeviceEvent {
    std::string event_id;                   ///< 全局唯一事件 ID
    std::string site_id;                    ///< 事件来源身份：现场
    std::string gateway_id;                 ///< 事件来源身份：网关
    uint32_t hci_index = 0;                 ///< 事件来源身份：HCI 适配器
    WirelessProtocol protocol = WirelessProtocol::Bluetooth;

    std::string device_address;             ///< BDADDR
    BtAddressType address_type = BtAddressType::Unknown;

    DeviceEventType event_type = DeviceEventType::LinkDisconnected;
    uint64_t timestamp_ms = 0;              ///< 事件时间（合并窗口中最早一条观测的时刻，墙钟）
    uint64_t monotonic_ns = 0;              ///< 事件时间（最早观测的 CLOCK_MONOTONIC，用于因果回填与排序）

    std::optional<int> rssi_at_event_dbm;   ///< nullopt = 未采集；不是 0

    uint8_t raw_reason_code = 0;            ///< 原始事实，无损保存
    DisconnectReason reason = DisconnectReason::Unknown;  ///< 归一化解释

    EvidenceSource source = EvidenceSource::Unknown;      ///< 定案证据来源
    std::string source_detail;                            ///< 定案证据的精确 hook 名

    /// 推断原因。Phase 1 恒为 nullopt——采集层不做诊断。
    std::optional<std::string> suspected_cause;

    /// 逃生舱：仅存审计性证据（raw_evidence 数组），不放查询关键数据
    std::string details_json;

    /// 序列化为单行 JSON（字段名 snake_case，与项目其余序列化保持一致）
    std::string toJson() const;
};

/**
 * @brief 把一条原始观测追加进事件的 raw_evidence 数组
 *
 * details_json 形如：{"raw_evidence":[{...},{"..."}]}
 * 该数组用于回答"这条业务事件是由哪些内核观测支撑的"。
 */
std::string appendRawEvidence(const std::string& details_json,
                              const RawBtObservation& obs);

// ============================================================================
// Phase 2 链路层领域模型扩展：复合主键、算法配置、三态状态机、设备画像
// ============================================================================

/**
 * @brief 复合无线设备键（统一身份）
 *
 * 杜绝以裸 MAC 为主键导致的跨网关、跨控制器或跨地址类型污染。
 * 主键区分 address_type，当前"设备"语义为观测身份。
 */
struct WirelessDeviceKey {
    std::string site_id;
    std::string gateway_id;
    uint32_t hci_index = 0;
    WirelessProtocol protocol = WirelessProtocol::Bluetooth;
    BtAddressType address_type = BtAddressType::Unknown;
    std::string device_address;  // 规范大写格式化 MAC

    bool operator<(const WirelessDeviceKey& o) const;
    bool operator==(const WirelessDeviceKey& o) const;
    bool operator!=(const WirelessDeviceKey& o) const { return !(*this == o); }

    std::string toString() const;
};

/**
 * @brief 射频采样观测
 */
struct RssiSample {
    int16_t rssi_dbm = -1000;
    uint64_t observed_at_ms = 0;   ///< 墙钟毫秒（展示/持久化）
    uint64_t monotonic_ns = 0;     ///< CLOCK_MONOTONIC 纳秒（因果判定/TTL）
    bool from_signal = true;       ///< true=PropertiesChanged 推送; false=轮询快照
};

/**
 * @brief 链路质量三态状态机
 */
enum class LinkQualityState : uint8_t {
    Learning = 0,   ///< 学习收敛期（样本不足 min_baseline_samples）
    Stable   = 1,   ///< 基线稳定，正常监控
    Degraded = 2    ///< 发生持续显著劣化（基线冻结，不更新）
};

/**
 * @brief 链路质量算法配置
 */
struct BtLinkQualityConfig {
    size_t min_baseline_samples = 10;          ///< 最小基线样本数
    size_t degrade_streak = 3;                 ///< 恶化判定所需连续坏样本数
    size_t recover_streak = 3;                 ///< 恢复判定所需连续好样本数
    int degrade_delta_db = 15;                 ///< 恶化门限（低于基线此值）
    int recover_delta_db = 8;                  ///< 恢复门限（回升到基线此值以内，保留 7dB 滞回区）
    uint64_t max_fresh_gap_ms = 15000;         ///< 相邻 fresh 样本允许的最大时间间隔（防稀疏触发）
    uint64_t rssi_event_ttl_ms = 10000;        ///< 断连前回填有效 RSSI 的最大 TTL（10s）
    uint64_t baseline_stale_after_ms = 86400000; ///< 长期离线重学周期（24h，可配置）
    uint32_t algorithm_version = 1;            ///< 算法版本
};

/**
 * @brief 设备链路基线画像（轻量版）
 *
 * 持久化到 device_baselines 表。去除派生统计（degraded_count 等按事件表聚合），
 * 杜绝双重事实来源导致的数据不一致。
 */
struct DeviceLinkProfile {
    WirelessDeviceKey key;
    std::optional<int16_t> baseline_rssi_dbm;  ///< 当前参考基线（中位数）
    std::optional<int16_t> min_seen_rssi_dbm;  ///< 观测到的最低 RSSI
    std::optional<int16_t> max_seen_rssi_dbm;  ///< 观测到的最高 RSSI
    size_t baseline_sample_count = 0;          ///< 当前基线实际使用的可信样本数
    uint64_t first_seen_ms = 0;                ///< 首次发现时间（墙钟）
    uint64_t last_seen_ms = 0;                 ///< 最近活跃时间（墙钟）
    LinkQualityState state = LinkQualityState::Learning;
    uint64_t updated_at_ms = 0;

    std::string toJson() const;
};

// ============================================================================
// 枚举 <-> 字符串 / 数值转换
// ============================================================================

const char* toString(WirelessProtocol v);
const char* toString(BtAddressType v);
const char* toString(DeviceEventType v);
const char* toString(DisconnectReason v);
const char* toString(EvidenceSource v);
const char* toString(LinkQualityState v);

/// 解析字符串；无法识别时返回 fallback
WirelessProtocol wirelessProtocolFromString(const std::string& s, WirelessProtocol fallback);
DeviceEventType deviceEventTypeFromString(const std::string& s, DeviceEventType fallback);
DisconnectReason disconnectReasonFromString(const std::string& s, DisconnectReason fallback);
LinkQualityState linkQualityStateFromString(const std::string& s, LinkQualityState fallback);

/// Phase 3a：启动回放需要从 device_events 重建事件对象，因此地址类型与证据来源
/// 也需要各自的逆映射。与上面五个解析函数同一约定：只解析本模块自己写出的字符串，
/// 大小写敏感，无法识别时返回 fallback 而不抛异常。
BtAddressType btAddressTypeFromString(const std::string& s, BtAddressType fallback);
EvidenceSource evidenceSourceFromString(const std::string& s, EvidenceSource fallback);

/**
 * @brief 把内核 HCI error code 映射为归一化断连原因
 *
 * 仅适用于 **HCI 域**的 reason（如 `hci_disconnect(conn, reason)` 的参数）。
 * 未识别的 code 返回 DisconnectReason::Other —— 调用方必须同时保留
 * raw_reason_code，避免 Other 成为信息黑洞。
 */
DisconnectReason disconnectReasonFromHciCode(uint8_t hci_code);

/**
 * @brief 把内核 mgmt 层 MGMT_DEV_DISCONN_* 原因映射为归一化断连原因
 *
 * **关键域区分（板端源码+寄存器 dump 双重实锤）**：
 * `mgmt_device_disconnected()` 的 `reason` 参数**不是 HCI error code**，
 * 而是内核 include/net/bluetooth/mgmt.h 的 MGMT_DEV_DISCONN_* 枚举：
 *
 *   0x00 MGMT_DEV_DISCONN_UNKNOWN
 *   0x01 MGMT_DEV_DISCONN_TIMEOUT
 *   0x02 MGMT_DEV_DISCONN_LOCAL_HOST
 *   0x03 MGMT_DEV_DISCONN_REMOTE
 *   0x04 MGMT_DEV_DISCONN_AUTH_FAILURE
 *   0x05 MGMT_DEV_DISCONN_LOCAL_HOST_SUSPEND
 *
 * 内核 `hci_to_mgmt_reason()` 做 HCI→MGMT 转换（如 HCI 0x16 本地断开 → 0x02）。
 * 若误当 HCI 码处理，0x02 会被映射成 Other（实测踩过：本机断开被记成 OTHER，
 * 而 btmon 同一次断连显示 "Connection Terminated By Local Host (0x16)"，
 * 语义其实是 LOCAL_HOST）。
 *
 * 归属哪种域由 EvidenceSource 判定：KernelMgmt 用本函数，KernelHci 用上面的
 * `disconnectReasonFromHciCode`。
 */
DisconnectReason disconnectReasonFromMgmtCode(uint8_t mgmt_code);

/**
 * @brief 按内核 link_type + LE 地址类型（HCI 域）判定 BtAddressType
 *
 * 内核 v5.15 语义（对照 mgmt.c 的 link_to_bdaddr()）：
 *   - link_type != LE_LINK(0x80)（即 ACL_LINK=0x01 等）→ BR/EDR
 *   - LE_LINK 且 addr_type == ADDR_LE_DEV_PUBLIC(0x00) → LE Public
 *   - LE_LINK 其余取值（RANDOM=0x01、*_RESOLVED=0x02/0x03）→ LE Random
 *     （resolved 地址在 mgmt 层也归为 RANDOM，因为实际用的是随机可解析地址）
 *
 * 注意：mgmt 的 addr_type 参数与 hci_conn.dst_type 都用这个 HCI 域取值
 * （0=public, 1=random, 2=public_resolved, 3=random_resolved），
 * **不是** mgmt API 的 BDADDR_* 域（BREDR=0/LE_PUBLIC=1/LE_RANDOM=2）。
 * 混淆会把 random 地址错标成 LE_PUBLIC（板端实测踩过）。
 */
BtAddressType btAddressTypeFromKernel(uint8_t link_type, uint8_t addr_type);

/// 内核 link_type 常量（include/net/bluetooth/hci.h），供调用方判断
constexpr uint8_t kKernelAclLink = 0x01;   ///< ACL_LINK
constexpr uint8_t kKernelLeLink  = 0x80;   ///< LE_LINK

// ============================================================================
// BDADDR 转换（字节序是关键，切勿自行手写）
// ============================================================================

/**
 * @brief 内核 bdaddr_t.b[6] -> "AA:BB:CC:DD:EE:FF"
 *
 * **内核 bdaddr_t 是反序存储的**：`b[0]` 是最低字节，人类可读/BlueZ 显示格式
 * 的最高字节在最前（对照 BlueZ 的 `ba2str()`，它按 b[5]..b[0] 顺序打印）。
 *
 * 这个坑在板端实测中真实踩到过：连接 `AA:BB:CC:DD:EE:01` 被记成
 * `01:EE:DD:CC:BB:AA`。所有 bdaddr 格式化一律走本函数，不要在各处重复手写。
 */
std::string formatBdaddr(const uint8_t bdaddr[6]);

/**
 * @brief "AA:BB:CC:DD:EE:FF" -> 内核 bdaddr_t.b[6]（反序填充）
 *
 * formatBdaddr 的逆运算。解析失败返回 false，out 内容不变。
 */
bool parseBdaddr(const std::string& mac, uint8_t out[6]);

}  // namespace weaknet_dbus
