/**
 * @file database_manager.hpp
 * @brief SQLite 历史数据持久化管理器
 *
 * 负责将 NetInfo 快照定期写入 SQLite，供客户端通过 D-Bus 历史查询接口取回。
 * 使用 prepared statement 避免 SQL 注入；忙时等待（busy_timeout=5000ms）避免多线程并发写冲突。
 *
 * 表结构（单表 snapshots）：
 *   id INTEGER PRIMARY KEY AUTOINCREMENT,
 *   timestamp TEXT NOT NULL,           -- ISO 8601
 *   iface TEXT NOT NULL,               -- 接口名
 *   rtt_ms INTEGER,                    -- RTT 延迟
 *   rssi_dbm INTEGER,                  -- Wi-Fi RSSI
 *   tcp_loss_rate REAL,                -- TCP 丢包率 %
 *   jitter_ms REAL,                    -- 抖动
 *   traffic_bps INTEGER,               -- 带宽
 *   score REAL,                        -- 综合质量评分
 *   quality TEXT                       -- 质量等级字符串
 */

#pragma once

#include <string>
#include <mutex>
#include <cstdint>

struct sqlite3;  ///< 前置声明 SQLite 句柄类型

namespace weaknet_dbus {

class NetInfo;  ///< 前置声明

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
     */
    bool insertSnapshot(const std::string& iface, const NetInfo& info, double score = 0.0);

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
     * @brief 清理过期快照
     * @param retention_days 保留天数（默认 7）
     * @return 删除行数，失败返回 -1
     */
    int cleanup(int retention_days = 7);

    /// 总记录数（SELECT COUNT(*) FROM snapshots）
    int64_t getRecordCount();

    /// 数据库元信息（文件名、SQLite 版本、表行数、文件大小）
    std::string getDbInfo();

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

    sqlite3* db_ = nullptr;       ///< SQLite 数据库句柄
    std::mutex write_mutex_;      ///< 保护所有写操作（SQLite 多线程安全的最简方案）
};

}  // namespace weaknet_dbus
