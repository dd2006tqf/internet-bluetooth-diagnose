/**
 * @file site_incident_correlator.hpp
 * @brief 区域级异常关联器（Phase 3a：把散落的设备事件聚合成 SiteIncident）
 *
 * 一句话职责：**当现场有多台设备在同一个时间窗口内同时出现合格异常时，
 * 产出一条 SiteIncident，并保留构成它的每一条事件的可追溯回链。**
 *
 * 数据流（严格单向）：
 *
 *   BtEventMonitor::flushToStore()
 *        ↓ WirelessDeviceEvent（Canonical Device Event）
 *   WirelessEventStore::recordEvent()          ← 落库、内存缓冲
 *        ↓ 同一事件（落库之后）
 *   SiteIncidentCorrelator::observe(event)     ← 本类
 *        ↓ 双门槛满足时产出/更新 SiteIncident
 *   site_incidents 表 (+ site_incident_events 回链)
 *
 * 四条设计要点（也是最容易被后来者改坏的地方）：
 *
 *   1. **动态活跃分母，不是全库设备数**
 *      受影响比例的分母是"关联窗口内出现过**任一**事件的设备数"，不是数据库里
 *      已知的设备总数。工业现场的设备画像会长期留存，一台三个月前离线、
 *      再未出现的设备会把比率永久稀释到永不达标——那时关联器就静默失效了。
 *
 *   2. **重启连续性靠回放，不靠内存**
 *      Incident 的生命周期长于进程。服务重启时若正处于 OPEN/ONGOING 的 incident
 *      必须通过回放 device_events 最近窗口恢复，否则"重启一次丢一起事故"。
 *      这是 site_incident_events 必须落库（而不仅是内存态）的根本原因。
 *
 *   3. **同一事件幂等**
 *      recordEvent 可能因重试/回放被重复调用。同一 event_id 只计一次，
 *      否则重复事件会把受影响设备数灌水到门槛之上，凭空造出一起假事故。
 *
 *   4. **事件驱动，不做轮询**
 *      关联窗口与静默期都以**事件时间**（WirelessDeviceEvent::timestamp_ms）为准，
 *      读时钟的轮询会让判定结果依赖于调用节奏。消费线程每轮 flush 之后调用一次
 *      tick() 收集结案结果即可。
 *
 * 锁纪律：全局加锁方向恒为
 *   （无）-> correlator.mutex_ -> DatabaseManager::mutex_
 * 绝不反向持有 db 锁再取 correlator 锁（与 WirelessEventStore 同一条纪律）。
 *
 * 线程安全：所有公开方法由内部 mutex_ 保护。写入方是 bt_events 消费线程，
 * 读取方是 D-Bus 工作线程 / CLI，无需外部加锁。
 */

#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "site_incident.hpp"
#include "wireless_event.hpp"

namespace weaknet_dbus {

class DatabaseManager;  ///< 前置声明

/**
 * @brief 关联器配置
 *
 * 默认值取自 Phase 3a 设计：60 秒关联窗口 + 30% 受影响比例，双门槛
 * （比例 ∧ 最少设备数）。单设备异常永远开不出事故——这正是
 * "一台设备坏了"与"这一片无线环境出了问题"的分界线。
 */
struct SiteIncidentConfig {
    /// 关联窗口（毫秒）：窗口内累计的合格异常参与同一 incident 判定
    uint64_t correlation_window_ms = 60000;
    /// 静默窗口（毫秒）：窗口内无新合格异常 → RESOLVED
    uint64_t quiet_window_ms = 60000;
    /// 受影响比例门槛（0.0–1.0）：受影响设备数 / 窗口内活跃设备数
    double min_affected_ratio = 0.3;
    /// 最少受影响设备数（绝对门槛）：与比例门槛同时满足才开事故
    size_t min_affected_devices = 2;
    /// 活跃设备记忆时长（毫秒）：窗口内出现过事件的设备计入分母的时长
    uint64_t active_device_memory_ms = 60000;
    /// Incident ID 前缀
    std::string incident_id_prefix = "sitinc";
    /// site_id：本关联器负责的现场身份。为空时取事件自带的 site_id。
    std::string site_id;
    /// 合格异常判定策略（"哪些事件算事故证据"这条产品语义的唯一来源）
    QualifyingAnomalyPolicy policy;
};

/**
 * @brief 区域级异常关联器
 *
 * 生命周期：
 *   SiteIncidentCorrelator correlator(db, cfg);
 *   correlator.recoverFromStore(now_ms);   // 服务启动：回放 device_events 恢复内存态
 *   correlator.observe(event);             // 运行期：每条 canonical event 投递一次
 *   correlator.tick(now_ms);               // 每轮 flush 后调用一次，收集结案结果
 */
class SiteIncidentCorrelator {
public:
    /**
     * @param db 数据库管理器；可为 nullptr（此时仅内存态，用于单元测试）。
     *           关联器不拥有 db 的生命周期。
     */
    explicit SiteIncidentCorrelator(DatabaseManager* db = nullptr,
                                    SiteIncidentConfig cfg = {});

    SiteIncidentCorrelator(const SiteIncidentCorrelator&) = delete;
    SiteIncidentCorrelator& operator=(const SiteIncidentCorrelator&) = delete;

    /**
     * @brief 观察一条 Canonical Device Event
     *
     * 所有事件（含非合格异常）都会更新活跃设备记忆——分母回答的是
     * "这片区域此刻有几台设备在被观测"。只有合格异常才参与事故判定：
     *   1. 若该现场已有活跃 incident 且在关联窗口内，吸收进它（不新开）
     *   2. 否则用动态分母重新评估双门槛，达标则开一条新 incident
     *
     * 结案的收集请用 tick()——本方法只返回**被本条事件触及**的 incident，
     * 不会把顺带结案的历史 incident 混进返回值。
     *
     * @return 新开 / 被吸收 / 状态推进的 incident；无任何变化时 nullopt
     */
    std::optional<SiteIncident> observe(const WirelessDeviceEvent& event);

    /// 当前活跃（OPEN/ONGOING）incident 快照（含受影响设备清单）
    std::vector<SiteIncident> activeIncidents() const;

    /// 自进程启动以来产出的 incident 总数（诊断用）
    uint64_t totalIncidents() const;

    /**
     * @brief 推进静默期：把 last_event_ms + quiet_window_ms 已过的活跃 incident 结案
     *
     * 关联器是事件驱动的，但"没有新异常"本身也是一种状态变化——静默期内不会
     * 有人调用 observe()，因此需要消费线程每轮主动推进一次。
     *
     * @return 本次被结案的 incident 列表（每条都已落库）
     */
    std::vector<SiteIncident> tick(uint64_t now_ms);

    /**
     * @brief 服务启动回放：从 device_events 最近窗口恢复关联器内存态
     *
     * 回放的是**事件**而不是 incident 行：跨重启连续性由"重放最近
     * correlation_window + quiet_window 的事件、重新推导活跃 incident"保证。
     * 重新推导出的 incident 带确定性 ID（由 site_id + started_at_ms 决定），
     * 落库为 UPSERT，因此回放天然幂等——不会出现"重启一次多一起事故"。
     *
     * @param now_ms 当前墙钟毫秒（回放窗口的右沿）
     * @return 回放后仍活跃（OPEN/ONGOING）的 incident 数量
     */
    size_t recoverFromStore(uint64_t now_ms);

    /**
     * @brief 查询持久化的 incident 列表（DB 为空时返回 "[]"）
     *
     * @param state   状态过滤（"OPEN"/"ONGOING"/"RESOLVED"），"" 表示不限
     * @param start_ms 起始时间（Unix 毫秒），0 表示不限
     * @param end_ms   结束时间（Unix 毫秒），0 表示不限
     * @param limit    最大条数
     * @param include_devices 是否附带受影响设备清单（关联查询 site_incident_events）
     */
    std::string queryPersisted(const std::string& state,
                               int64_t start_ms,
                               int64_t end_ms,
                               int limit = 100,
                               bool include_devices = true) const;

private:
    /// 窗口内的一条合格异常样本（门槛判定的输入）
    struct QualifyingSample {
        uint64_t ts_ms = 0;
        std::string event_id;
        std::string device_key;
    };

    /// 一条正在累积的 incident 内存态
    struct ActiveIncident {
        SiteIncident incident;
        /// event_id -> device_key。既是幂等去重的唯一依据，
        /// 也是 site_incident_events 回链所需的精确映射（1:1，不靠猜）。
        std::map<std::string, std::string> event_device;

        /// 受影响设备数（去重后的 device_key 个数）
        size_t deviceCount() const;
    };

    /// 设备活跃记忆：device_key -> 最近一次被观测到的时间
    using ActiveDeviceMemory = std::map<std::string, uint64_t>;

    /// observe 的无锁实现（recoverFromStore 复用，避免重入加锁）
    std::optional<SiteIncident> observeLocked(const WirelessDeviceEvent& event,
                                              uint64_t now_ms);

    /// tick 的无锁实现
    std::vector<SiteIncident> tickLocked(uint64_t now_ms);

    /// 生成 incident_id：<prefix>_<site>_<started_at_ms>（确定性，回放幂等）
    std::string makeIncidentId(const std::string& site_id, uint64_t started_at_ms) const;

    /// 动态分母：活跃设备记忆中仍然新鲜的设备数
    size_t activeDeviceCount(uint64_t now_ms) const;

    /// 复合设备键（含地址类型与协议，杜绝跨地址类型污染）
    static std::string deviceKeyOf(const WirelessDeviceEvent& event);

    /// 把一条事件并入活跃 incident（更新 last_event/state/受影响集合）
    void mergeInto(ActiveIncident& active, const WirelessDeviceEvent& event);

    /// 用窗口内的合格异常样本尝试开一条新 incident；达标返回 true
    bool tryOpenLocked(const WirelessDeviceEvent& event, uint64_t now_ms, SiteIncident& out);

    /// 结案一条活跃 incident 并落库
    SiteIncident resolveLocked(ActiveIncident& active);

    /// 落库 incident 本体 + 事件回链（调用方须已持有 mutex_）
    void persistLocked(const ActiveIncident& active);

    DatabaseManager* db_;   ///< 不拥有所有权
    SiteIncidentConfig cfg_;

    uint64_t total_incidents_ = 0;

    mutable std::mutex mutex_;
    /// 活跃 incident：key 为 site_id（Phase 3a 运行模型 1 Gateway = 1 Site）
    std::map<std::string, ActiveIncident> active_;
    /// 窗口内的合格异常样本（门槛判定输入，随 tick 过期淘汰）
    std::deque<QualifyingSample> recent_qualifying_;
    /// 活跃设备记忆（动态分母的唯一来源）
    ActiveDeviceMemory device_memory_;
};

}  // namespace weaknet_dbus
