/**
 * @file database_manager.hpp
 * @brief SQLite 历史数据持久化管理器
 *
 * 负责将 NetInfo 快照定期写入 SQLite，供客户端通过 D-Bus 历史查询接口取回。
 * 使用 prepared statement 避免 SQL 注入；忙时等待（busy_timeout=5000ms）避免多线程并发写冲突。
 *
 * 表结构（单表 network_history）：
 *   ts TEXT NOT NULL,                  -- ISO 8601 快照时间
 *   iface TEXT NOT NULL,               -- 接口名
 *   rtt_ms/jitter_ms/rssi_dbm REAL,    -- 指标值；stale 时为 SQL NULL
 *   rssi_status/rtt_status/jitter_status/tcp_loss_status/traffic_status TEXT,
 *                                       -- valid/unavailable/timeout/stale
 *   tcp_loss REAL, traffic_bps/pps/flows, -- 指标值
 *   data_version INTEGER,              -- 数据格式版本；当前为 2
 *   *_sample_ts INTEGER                -- 各指标 Unix 毫秒采样时间 */

#pragma once

#include <string>
#include <mutex>
#include <cstdint>
#include <optional>
#include <vector>

struct sqlite3;  ///< 前置声明 SQLite 句柄类型

namespace weaknet_dbus {

class NetInfo;  ///< 前置声明
struct NetworkQualityResult;  ///< 前置声明
struct WirelessDeviceEvent;  ///< 前置声明（Phase 3a 启动回放）
struct SiteIncident;         ///< 前置声明（Phase 4a 无线事实上行）
struct DeviceLinkProfile;    ///< 前置声明（Phase 4a 无线事实上行）

/**
 * @brief SQLite 历史数据持久化管理器
 *
 * 不可拷贝（sqlite3* 资源所有权语义）。
 * 线程安全：所有写入操作通过 write_mutex_ 保护。
 * 读取操作（queryHistory）内部可能开启自己的访问模式，但当前实现也走 write_mutex_ 简化处理。
 */
class DatabaseManager {
public:
    /**
     * @brief 打开（或创建）SQLite 数据库文件
     *
     * @param db_path 数据库文件完整路径（目录必须已存在）
     */
    explicit DatabaseManager(const std::string& db_path);
    ~DatabaseManager();

    DatabaseManager(const DatabaseManager&) = delete;
    DatabaseManager& operator=(const DatabaseManager&) = delete;

    /// 数据库连接是否成功建立
    bool isOpen() const;

    /**
     * @brief 写入一条快照记录
     *
     * 由 history_persistence_thread 每 5 分钟对每个 NetInfo 调用一次。
     * 内部使用 prepared statement 避免 SQL 注入。
     *
     * @param iface  接口名（NetInfo::ifName()）
     * @param info   完整 NetInfo 快照
     * @param score  综合质量评分（由 NetworkQualityAssessor 计算，0.0 表示未评分）
     * @param assessment_profile 写出该行时的评估 Profile 字符串
     *        （"NETWORK_ONLY"/"INTERNET_ACCESS"）。此前恒写死 INTERNET_ACCESS，
     *        设备实际跑 NETWORK_ONLY 时历史审计元数据是错的。
     */
    bool insertSnapshot(const std::string& iface, const NetInfo& info,
                        const NetworkQualityResult& overall,
                        uint64_t generation = 0,
                        int64_t snapshot_ts_ms = 0,
                        int64_t rtt_sample_ts = 0,
                        int64_t rssi_sample_ts = 0,
                        int64_t jitter_sample_ts = 0,
                        int64_t tcp_loss_sample_ts = 0,
                        int64_t traffic_sample_ts = 0,
                        const char* assessment_profile = "UNSPECIFIED");

    /**
     * @brief 查询历史快照，返回 JSON 数组字符串
     *
     * @param interface  接口过滤："" 表示所有网卡
     * @param start      起始时间（ISO 8601），"" 表示不限
     * @param end        结束时间（ISO 8601），"" 表示不限
     * @param limit      最大返回行数（默认 100）
     * @return JSON 数组（失败时返回 "[]"）
     */
    std::string queryHistory(const std::string& interface,
                             const std::string& start,
                             const std::string& end,
                             int limit = 100);

    /**
     * @brief 写入蓝牙与音频时序快照
     */
    bool insertBtSnapshot(const std::string& adapter_mac,
                          const std::string& device_mac,
                          const std::string& device_name,
                          bool connected,
                          int16_t rssi_dbm,
                          double distance_m,
                          bool audio_active,
                          double quality_score,
                          bool suspected_stall,
                          uint64_t bytes_per_sec = 0,
                          uint64_t max_gap_ms = 0);

    /**
     * @brief 查询蓝牙历史时序数据，返回 JSON 数组字符串
     * @param device_mac 设备 MAC 过滤，"" 表示所有设备
     * @param start      起始时间（ISO 8601），"" 表示不限
     * @param end        结束时间（ISO 8601），"" 表示不限
     * @param limit      最大行数
     * @return JSON 数组字符串
     */
    std::string queryBtHistory(const std::string& device_mac,
                               const std::string& start,
                               const std::string& end,
                               int limit = 100);

    /**
     * @brief 写入一条规范化无线设备事件
     *
     * 由 WirelessEventStore 调用。与 insertBtSnapshot 的语义区别：
     * 后者写的是"周期采样快照"，本方法写的是"事件"（只在断连/发现/劣化时发生）。
     *
     * 关于 NULL 语义：rssi_dbm 用 optional 表达"未采集"，写库为 SQL NULL；
     * 用 0 冒充未采集会污染后续 RSSI 统计。suspected_cause 同理（Phase 1 恒为空）。
     *
     * @param event_id       全局唯一事件 ID
     * @param ts_ms          事件时刻（Unix 毫秒）
     * @param site_id        现场身份（事件来源）
     * @param gateway_id     网关身份（事件来源）
     * @param protocol       协议族字符串（如 "BLUETOOTH"）
     * @param device_address 设备地址
     * @param address_type   地址类型字符串（如 "LE_PUBLIC"）
     * @param hci_index      HCI 适配器序号
     * @param event_type     事件类型字符串（如 "LINK_DISCONNECTED"）
     * @param rssi_dbm       事件时刻 RSSI；nullopt 表示未采集（写 NULL）
     * @param raw_reason_code 内核原始 HCI reason（无损保存）
     * @param reason         归一化原因字符串
     * @param source         证据来源字符串
     * @param source_detail  精确 hook 名
     * @param suspected_cause 推断原因；nullopt 写 NULL
     * @param details_json   审计性证据（raw_evidence 数组）
     * @return true 写入成功
     */
    bool insertDeviceEvent(const std::string& event_id,
                           int64_t ts_ms,
                           const std::string& site_id,
                           const std::string& gateway_id,
                           const std::string& protocol,
                           const std::string& device_address,
                           const std::string& address_type,
                           uint32_t hci_index,
                           const std::string& event_type,
                           const std::optional<int>& rssi_dbm,
                           uint8_t raw_reason_code,
                           const std::string& reason,
                           const std::string& source,
                           const std::string& source_detail,
                           const std::optional<std::string>& suspected_cause,
                           const std::string& details_json);

    /**
     * @brief 查询规范化无线设备事件，返回 JSON 数组字符串
     *
     * @param device_address 设备地址过滤，"" 表示所有设备
     * @param event_type     事件类型过滤（如 "LINK_DISCONNECTED"），"" 表示所有类型
     * @param start_ms       起始时间（Unix 毫秒），0 表示不限
     * @param end_ms         结束时间（Unix 毫秒），0 表示不限
     * @param limit          最大行数
     * @return JSON 数组字符串（失败时返回 "[]"）
     */
    std::string queryDeviceEvents(const std::string& device_address,
                                  const std::string& event_type,
                                  int64_t start_ms,
                                  int64_t end_ms,
                                  int limit = 100);

    /**
     * @brief 插入或更新设备链路基线画像（复合主键，UPSERT）
     */
    bool upsertDeviceBaseline(const std::string& site_id,
                              const std::string& gateway_id,
                              uint32_t hci_index,
                              const std::string& protocol,
                              const std::string& address_type,
                              const std::string& device_address,
                              const std::optional<int16_t>& baseline_rssi_dbm,
                              const std::optional<int16_t>& min_seen_rssi_dbm,
                              const std::optional<int16_t>& max_seen_rssi_dbm,
                              size_t baseline_sample_count,
                              int64_t first_seen_ms,
                              int64_t last_seen_ms,
                              const std::string& state,
                              int64_t updated_at_ms);

    /**
     * @brief 查询设备链路基线画像列表，返回 JSON 数组字符串
     */
    std::string queryDeviceBaselines(const std::string& device_address = "",
                                    int limit = 100);

    // ------------------------------------------------------------------
    // Phase 3a：区域级异常事件（SiteIncident）
    //
    // 为什么必须有这两张表（而不是只放内存）：
    //   1. incident 的生命周期长于进程——它可能有数分钟到数小时的跨度，
    //      而网关会被重启/升级。只放内存 = 重启一次丢一起事故。
    //   2. 证据回链必须可查：用户问"凌晨 3 点影响了哪些设备"时，答案不是
    //      "若干设备"，而是"这几条具体事件"。文本塞进 incident 行做不到按
    //      event_id 精确回溯，也无法保证不重复计数。
    // ------------------------------------------------------------------

    /**
     * @brief 插入或更新一条区域级异常事件（按 incident_id UPSERT）
     *
     * UPSERT 而非 INSERT：关联器在 incident 的每一次状态推进（新开 / 吸收
     * 新证据 / 结案）都会写回同一行；服务重启后的回放也会重新推导出
     * **同一个** incident_id（确定性 ID），必须更新而不是插新行——
     * 否则"重启一次多一起事故"。
     *
     * NULL 语义：`resolved_at_ms` 用 optional 表达"仍在活跃"（写 SQL NULL）；
     * `suspected_cause` 同理（Phase 3a 恒为 NULL——关联层不做根因推断）。
     * 用 0 冒充"未结案"会让"1970 年结案"这种假事实进入历史。
     */
    bool upsertSiteIncident(const std::string& incident_id,
                            const std::string& site_id,
                            const std::string& gateway_id,
                            int64_t started_at_ms,
                            int64_t last_event_ms,
                            const std::optional<int64_t>& resolved_at_ms,
                            int64_t affected_devices,
                            const std::string& state,
                            const std::optional<std::string>& suspected_cause);

    /**
     * @brief 建立 incident 与其证据事件的回链（1:N）
     *
     * 复合主键 (incident_id, event_id) 上的 INSERT OR IGNORE：回放与重试天然幂等，
     * 同一事件不会被重复计入同一起事故。
     *
     * @return true 写入成功或已存在（幂等）；false SQL 失败
     */
    bool insertSiteIncidentEvent(const std::string& incident_id,
                                 const std::string& event_id);

    /**
     * @brief 查询区域级异常事件，返回 JSON 数组字符串
     *
     * @param state   状态过滤（"OPEN"/"ONGOING"/"RESOLVED"），"" 表示不限
     * @param start_ms 起始时间（Unix 毫秒，按 last_event_ms 过滤），0 表示不限
     * @param end_ms   结束时间（Unix 毫秒），0 表示不限
     * @param limit    最大条数
     * @param include_devices 是否附带受影响设备清单（关联 site_incident_events
     *                  → device_events 派生，不额外存第二份事实）
     * @return JSON 数组字符串（失败时返回 "[]"）
     */
    std::string querySiteIncidents(const std::string& state,
                                   int64_t start_ms,
                                   int64_t end_ms,
                                   int limit = 100,
                                   bool include_devices = true);

    /**
     * @brief 该现场最近一次 incident 的结案时刻（Unix 毫秒），无记录返回 0
     *
     * 供关联器启动回放使用：只回放这一时刻之后的事件，避免把已结案的
     * incident 重新拉回活跃态（"重启复活旧事故"）。
     */
    int64_t queryLatestIncidentResolvedAt(const std::string& site_id);

    /**
     * @brief 回放窗口内的规范化设备事件（时间正序），供关联器重建内存态
     *
     * 与 queryDeviceEvents 的区别是**排序方向**：查询出口按时间倒序给用户看，
     * 回放必须按时间正序重演决策过程。两个方向刻意做成两个方法，
     * 避免"加一个参数改变排序"这种让调用方踩坑的隐式开关。
     *
     * @param start_ms 起始时间（Unix 毫秒，闭区间）
     * @param end_ms   结束时间（Unix 毫秒，闭区间）
     */
    std::vector<WirelessDeviceEvent> loadDeviceEventsForReplay(int64_t start_ms,
                                                               int64_t end_ms);

    /**
     * @brief 事故本体的类型化读取（时间正序），供无线事实上行使用
     *
     * 与 querySiteIncidents（JSON 出口，时间倒序）刻意分开：上行需要的是
     * 类型化对象与**正序**（回放/补发的确定性顺序），而查询出口服务的是
     * "按时间倒序看最近发生了什么"。
     *
     * @param start_ms 起始事实时间（闭区间，0 表示不限）
     * @param limit    最大条数（<=0 时取默认 200）
     */
    std::vector<SiteIncident> loadSiteIncidentsSince(int64_t start_ms, int limit = 200);

    /**
     * @brief 事故 → 证据事件回链（site_incident_events）
     *
     * 证据可追溯性的载体：上行必须随事故本体携带，否则云端诊断无法枚举
     * 支撑该事故的具体事件（诊断 bundle 的 qualifying_events）。
     */
    std::vector<std::string> loadIncidentEvidenceEventIds(const std::string& incident_id);

    /**
     * @brief 设备链路基线画像的类型化读取（按 updated_at 正序）
     *
     * 上行要求**稳定顺序**：游标按 last_seen_ms 过滤，若读取顺序不稳，
     * 同一条画像在两轮之间可能被跳过或重复（重复无害但浪费流量）。
     * @param since_updated_at_ms 只取 updated_at 大于该值的画像（0 表示不限）
     */
    std::vector<DeviceLinkProfile> loadDeviceBaselinesSince(int64_t since_updated_at_ms,
                                                            int limit = 2000);

    /**
     * @brief 指定时间之后出现过的最大事件时间（用于建立初始上行游标）
     *
     * 与 loadDeviceEventsForReplay 配对：首次启动时游标为 0 会把历史事件
     * 全量重发（云端幂等，安全但浪费）。调用方可先取 max(ts) 作为起点。
     * 无记录时返回 0。
     */
    int64_t queryMaxDeviceEventTs();

    /**
     * @brief 清理过期快照
     * @param retention_days 保留天数（默认 7）
     * @return 删除行数，失败返回 -1
     */
    int cleanup(int retention_days = 7);

    /// 总记录数（SELECT COUNT(*) FROM snapshots）
    int64_t getRecordCount();

    /// 数据库元信息（文件名、SQLite 版本、表行数、文件大小）
    std::string getDbInfo();

    /// 数据质量统计（只读，不修改历史数据）
    std::string getQualityReport();



private:
    /**
     * @brief 内部执行原始 SQL（不支持参数绑定，用于 CREATE TABLE / PRAGMA）
     * @return true 执行成功
     */
    bool exec(const std::string& sql);

    /**
     * @brief 确保 snapshots 表存在（首次打开数据库时调用）
     * @return true 表已存在或创建成功
     */
    bool ensureSchema();

    /// 内部无锁获取总记录数，供已持有 mutex_ 的方法调用
    int64_t getRecordCountLocked();

    sqlite3* db_ = nullptr;       ///< SQLite 数据库句柄
    mutable std::mutex mutex_;    ///< 统一保护所有 SQLite 操作（读写操作全覆盖）
};

}  // namespace weaknet_dbus
