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

struct sqlite3;  ///< 前置声明 SQLite 句柄类型

namespace weaknet_dbus {

class NetInfo;  ///< 前置声明
struct NetworkQualityResult;  ///< 前置声明

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
     * @param start_ms       起始时间（Unix 毫秒），0 表示不限
     * @param end_ms         结束时间（Unix 毫秒），0 表示不限
     * @param limit          最大行数
     * @return JSON 数组字符串（失败时返回 "[]"）
     */
    std::string queryDeviceEvents(const std::string& device_address,
                                  int64_t start_ms,
                                  int64_t end_ms,
                                  int limit = 100);

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
