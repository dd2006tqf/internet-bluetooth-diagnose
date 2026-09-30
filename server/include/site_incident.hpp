/**
 * @file site_incident.hpp
 * @brief 区域级异常事件领域模型（Phase 3a：SiteIncident）
 *
 * 本文件定义《产品定位：工业现场无线信号检测与诊断网关》中**区域层**的最小领域模型：
 *
 *   WirelessDeviceEvent（设备层：不可变事实）
 *        ↓ SiteIncidentCorrelator（时空关联）
 *   SiteIncident（区域层：持续解释）
 *
 * 四条硬约束（修改前必须读懂）：
 *
 *   1. Event 是不可变事实，Incident 是持续解释，两者生命周期不同
 *      同一次物理断连永远只对应一条 DeviceEvent；而"这一片无线环境出了问题"
 *      是一个**有开始、有延续、有结束**的解释过程。因此 Incident 单独建模，
 *      带自己的状态机（OPEN → ONGOING → RESOLVED）与时间戳，
 *      绝不用"把若干 event 打上同一个 tag"来冒充。详见产品定位文档第四节。
 *
 *   2. 采集与关联层永不产出推断
 *      `suspected_cause` 恒为 nullopt。关联器只回答"有没有多台设备在同时段异常、
 *      分别是谁、证据是哪几条事件"，绝不回答"为什么"——根因推断属于诊断器
 *      （Phase 4+）。`affected_devices` 是事实（几台设备同时异常），
 *      `suspected_cause` 是推断，两者永不合并。
 *
 *   3. 正常断开不是事故
 *      用户关机、主动断连（RemoteUserTerminated / LocalHostTerminated）是计划内行为。
 *      若把它们计入，每天下班整片设备关机必然误报一起"区域事故"，产品可信度直接归零。
 *
 *   4. 时间语义统一用墙钟毫秒
 *      Incident 需要跨进程重启延续、需要与 device_events.ts 对齐、需要被用户按
 *      时间检索，因此关联窗口一律以 WirelessDeviceEvent::timestamp_ms 为准，
 *      而不是 CLOCK_MONOTONIC。单调时钟仍在 DeviceEvent 内部保留用于因果回填。
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "wireless_event.hpp"

namespace weaknet_dbus {

// ============================================================================
// Incident 生命周期
// ============================================================================

/**
 * @brief 区域事件的生命周期状态
 *
 *   OPEN      刚刚达到关联门槛，正在成形（首条受影响设备集合）
 *   ONGOING   窗口内继续吸收到新的合格异常，仍在延续
 *   RESOLVED  静默期（quiet_window_ms）内无新合格异常，已结案
 *
 * OPEN 与 ONGOING 都是"活跃"状态，只有 RESOLVED 是终态。
 * 状态迁移严格单向：OPEN → ONGOING → RESOLVED，绝不回退
 * （回退意味着"已结案的事故又活了"，会让同一段历史出现两个解释）。
 */
enum class IncidentState : uint8_t {
    Open     = 0,
    Ongoing  = 1,
    Resolved = 2
};

const char* toString(IncidentState state);

/// 解析字符串；无法识别时返回 fallback
IncidentState incidentStateFromString(const std::string& s, IncidentState fallback);

// ============================================================================
// 合格异常判定
// ============================================================================

/**
 * @brief 什么算"合格异常"（区域事故的候选证据）
 *
 * 该策略是关联器的第一道门：只有通过它的 DeviceEvent 才能进入时空聚合。
 * 独立成结构体而不是藏在关联器里，是为了让"哪些事件算事故证据"这条产品语义
 * 可被单独评审与测试，而不是散落在相关代码的 if 里。
 *
 * 当前判定：
 *   - `LinkDisconnected` 且原因**不是**计划内断开（RemoteUserTerminated /
 *     LocalHostTerminated）→ 合格
 *   - `LinkDegraded` → 合格（链路劣化本身就是链路层异常）
 *   - 其余类型（DeviceAppeared / DeviceLost / LinkConnected / LinkRecovered /
 *     ConnectionAttempt）→ 不合格
 *
 * 注意：`reason == Unknown` 的断连**算**合格异常。语义是"已确认断开、
 * 但原因不在已知分类内"——未知不是"正常"，把它排除会让故障静默消失
 * （对应 wireless_event.hpp 中"Other/Unknown 不能成为信息黑洞"的同一条纪律）。
 */
struct QualifyingAnomalyPolicy {
    /// 是否把非计划断连计为合格异常
    bool include_disconnected = true;
    /// 是否把链路劣化计为合格异常
    bool include_degraded = true;

    /**
     * @brief 判定一条 Canonical Device Event 是否为合格异常
     */
    bool qualify(const WirelessDeviceEvent& event) const;

    /**
     * @brief 是否为计划内（用户主动/正常）断开
     *
     * 用户关机、主动断开连接的两种归一化原因。这两类不计入区域事故，
     * 否则"每天下班关机"会被当成区域无线故障。
     */
    static bool isPlannedTermination(DisconnectReason reason);
};

// ============================================================================
// 区域异常事件
// ============================================================================

/**
 * @brief 区域级异常事件（SiteIncident）
 *
 * 一行 SiteIncident 表示"某时段内该现场有多台设备同时出现合格异常"这一解释。
 * 它自身不携带证据明细——构成它的事件通过 `site_incident_events` 表回链，
 * 每条都能追到 `device_events.event_id`（可追溯性是工业诊断的基本要求）。
 *
 * 字段语义：
 *   - `started_at_ms`   本 incident 所覆盖的最早一条合格异常的时刻
 *                       （不是"发现时刻"——现实事故从最早异常就开始）
 *   - `last_event_ms`   最近一条被吸收的合格异常的时刻（滑动窗口右沿）
 *   - `resolved_at_ms`  `last_event_ms + quiet_window_ms`，即"确定静默"的时刻；
 *                       活跃期内为 nullopt（DB 为 NULL，不用 0 冒充）
 *   - `affected_devices` 受影响设备**数量**（事实）。设备**清单**由
 *                       `site_incident_events` 关联查询派生，不在此重复存储，
 *                       避免同一事实有两个来源。
 *   - `suspected_cause` 推断原因，恒为 nullopt——采集与关联层不做诊断
 */
struct SiteIncident {
    std::string incident_id;                  ///< 全局唯一 ID（含实例随机码，跨重启不碰撞）
    std::string site_id;                      ///< 现场身份（事件来源）
    std::string gateway_id;                   ///< 网关身份（事件来源）

    uint64_t started_at_ms = 0;               ///< 覆盖的最早合格异常时刻（墙钟毫秒）
    uint64_t last_event_ms = 0;               ///< 最近吸收的合格异常时刻（墙钟毫秒）
    std::optional<uint64_t> resolved_at_ms;   ///< 结案时刻；活跃期为 nullopt

    size_t affected_devices = 0;              ///< 受影响设备数（去重后）
    IncidentState state = IncidentState::Open;

    /// 推断原因。Phase 3a 恒为 nullopt——关联器只做时空聚合，不做根因推断。
    std::optional<std::string> suspected_cause;

    /// 受影响设备清单（派生字段）：仅在查询/快照时由 site_incident_events 关联填充，
    /// **不是**第二个事实来源。本结构体内存态中通常为空。
    std::vector<std::string> affected_device_ids;
};

}  // namespace weaknet_dbus
