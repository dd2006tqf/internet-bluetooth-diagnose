/**
 * @file database_manager.cpp
 * @brief SQLite 历史数据持久化管理器 — 网络质量快照持久化存储与查询
 *
 * 模块职责：
 *   - 打开/创建 SQLite 数据库（WAL 模式，多线程安全 FULLMUTEX）
 *   - 自动建表 ensureSchema()，创建 network_history 表 + 时间戳/接口索引
 *   - insertSnapshot() 将 NetInfo + 综合评分落库（参数绑定防 SQL 注入）
 *   - queryHistory() 支持按接口、时间范围、数量限制查询，返回 JSON 数组
 *   - cleanup() 按保留天数过期清理旧记录
 *   - getDbInfo() 返回数据库元信息（记录数/大小/时间范围）
 *
 * 表结构（network_history）：
 *   ┌──────────────┬────────────┬──────────────────────────────────────┐
 *   │ 列名          │ 类型        │ 说明                                   │
 *   ├──────────────┼────────────┼──────────────────────────────────────┤
 *   │ id           │ INTEGER PK │ 自增主键                               │
 *   │ ts           │ TEXT NN    │ ISO8601 时间戳（YYYY-MM-DDTHH:MM:SS） │
 *   │ iface        │ TEXT NN    │ 网卡名（wlan0/eth0 等）               │
 *   │ rtt_ms       │ INTEGER    │ RTT（毫秒，-1 表示未测量）             │
 *   │ jitter_ms    │ REAL       │ 抖动（毫秒，-1 表示未测量）            │
 *   │ rssi_dbm     │ INTEGER    │ Wi-Fi RSSI（dBm，-1000 表示未测量）    │
 *   │ tcp_loss     │ REAL       │ TCP 丢包率（%，-1 表示未测量）         │
 *   │ quality      │ TEXT       │ LinkQuality 枚举字符串                │
 *   │ score        │ REAL       │ 综合质量评分（0~100）                 │
 *   │ traffic_bps  │ INTEGER    │ 当前接口流量 bps                      │
 *   │ traffic_pps  │ INTEGER    │ 当前接口流量 pps                      │
 *   │ flows        │ INTEGER    │ 活跃流数量                            │
 *   └──────────────┴────────────┴──────────────────────────────────────┘
 *   索引：idx_history_ts(ts)、idx_history_iface(iface) — 加速时间范围与接口过滤查询
 *
 * 写入频率与清理策略：
 *   - 写入频率：由 Server 主循环控制（通常 3~10 秒/次，每次一行 per 活跃接口）
 *   - 清理策略：cleanup(retention_days) 按 ts < datetime('now', '-N days') DELETE
 *     默认由外部定时器（如 history_query_tool --cleanup）每周触发，
 *     也可由服务端内置周期清理
 *
 * SQLite 连接配置：
 *   - journal_mode=WAL        支持并发读写，读取不受写锁阻塞
 *   - busy_timeout=5000ms     高频写入时避免 SQLITE_BUSY 死锁
 *   - synchronous=NORMAL      WAL 模式下安全性足够，写性能最优
 *   - cache_size=-2048 pages  约 2MB 缓存，加速查询
 *   - FULLMUTEX 打开标志      多线程安全（比 SERIALIZED 稍快）
 */

#include "database_manager.hpp"
#include "net_info.hpp"
#include "logger.hpp"
#include "utils/json_escape.hpp"
#include <sqlite3.h>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <ctime>
#include <filesystem>

namespace weaknet_dbus {

// ---- 辅助函数 ----

static std::string currentTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm tm_now;
    localtime_r(&time_t_now, &tm_now);
    std::ostringstream oss;
    oss << std::put_time(&tm_now, "%Y-%m-%dT%H:%M:%S");
    return oss.str();
}

static std::string qualityToString(LinkQuality q) {
    switch (q) {
        case LinkQuality::Good:    return "GOOD";
        case LinkQuality::Fair:    return "FAIR";
        case LinkQuality::Poor:    return "POOR";
        case LinkQuality::Bad:     return "BAD";
        default:                   return "UNKNOWN";
    }
}

// ---- SQLite 回调 ----

struct QueryContext {
    std::string result;
    bool first = true;
};

static int queryCallback(void* data, int argc, char** argv, char** /*colNames*/) {
    auto* ctx = static_cast<QueryContext*>(data);
    if (ctx->first) {
        ctx->result = "[";
        ctx->first = false;
    } else {
        ctx->result += ",";
    }

    ctx->result += "{";
    for (int i = 0; i < argc; ++i) {
        if (i > 0) ctx->result += ",";
        ctx->result += "\"";
        ctx->result += weaknet_utils::escapeJsonString(argv[i] ? argv[i] : "null");
    }
    ctx->result += "}";

    return 0;
}

// 使用预定义列名的回调
struct HistoryRow {
    std::string ts, iface, quality;
    int rtt_ms = -1, rssi_dbm = -1000, traffic_pps = 0, flows = 0;
    double jitter_ms = -1, tcp_loss = -1, score = 0;
    int64_t traffic_bps = 0;
};

struct HistoryCallbackCtx {
    std::vector<HistoryRow> rows;
};

static int historyQueryCallback(void* data, int argc, char** argv, char** /*colNames*/) {
    auto* ctx = static_cast<HistoryCallbackCtx*>(data);
    HistoryRow row;
    if (argv[0]) row.ts = argv[0];
    if (argv[1]) row.iface = argv[1];
    if (argv[2]) row.rtt_ms = atoi(argv[2]);
    if (argv[3]) row.jitter_ms = atof(argv[3]);
    if (argv[4]) row.rssi_dbm = atoi(argv[4]);
    if (argv[5]) row.tcp_loss = atof(argv[5]);
    if (argv[6]) row.quality = argv[6];
    if (argv[7]) row.score = atof(argv[7]);
    if (argv[8]) row.traffic_bps = atoll(argv[8]);
    if (argv[9]) row.traffic_pps = atoi(argv[9]);
    if (argv[10]) row.flows = atoi(argv[10]);
    ctx->rows.push_back(std::move(row));
    return 0;
}

static int countCallback(void* data, int /*argc*/, char** argv, char** /*colNames*/) {
    auto* count = static_cast<int64_t*>(data);
    if (argv[0]) *count = atoll(argv[0]);
    return 0;
}

// ---- DatabaseManager 实现 ----

DatabaseManager::DatabaseManager(const std::string& db_path) {
    // 确保数据库目录存在
    auto dir_pos = db_path.find_last_of('/');
    if (dir_pos != std::string::npos) {
        std::string dir = db_path.substr(0, dir_pos);
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            LOG_ERROR(LogModule::SYSTEM, "DatabaseManager: failed to create directory " << dir << ": " << ec.message());
        }
    }

    int rc = sqlite3_open_v2(db_path.c_str(), &db_,
                              SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                              nullptr);
    if (rc != SQLITE_OK) {
        LOG_ERROR(LogModule::SYSTEM, "DatabaseManager: failed to open " << db_path << ": " << sqlite3_errmsg(db_));
        db_ = nullptr;
        return;
    }

    // 启用 WAL 模式（并发读写性能更好）
    exec("PRAGMA journal_mode=WAL");
    // 设置合理的缓存大小（默认 2MB）
    exec("PRAGMA cache_size=-2048");
    // 同步模式：NORMAL 在 WAL 下安全性足够
    exec("PRAGMA synchronous=NORMAL");
    // 设置忙超时 5 秒，防止高频写入时死锁
    exec("PRAGMA busy_timeout = 5000");

    if (!ensureSchema()) {
        LOG_ERROR(LogModule::SYSTEM, "DatabaseManager: schema creation failed");
        sqlite3_close(db_);
        db_ = nullptr;
        return;
    }

    LOG_INFO(LogModule::SYSTEM, "DatabaseManager: opened " << db_path);
}

DatabaseManager::~DatabaseManager() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool DatabaseManager::isOpen() const {
    return db_ != nullptr;
}

bool DatabaseManager::exec(const std::string& sql) {
    if (!db_) return false;
    char* err = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        LOG_ERROR(LogModule::SYSTEM, "DatabaseManager::exec SQL error: " << (err ? err : "unknown"));
        sqlite3_free(err);
        return false;
    }
    return true;
}

bool DatabaseManager::ensureSchema() {
    const char* schema = R"(
        CREATE TABLE IF NOT EXISTS network_history (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            ts          TEXT    NOT NULL,
            iface       TEXT    NOT NULL,
            rtt_ms      INTEGER DEFAULT -1,
            jitter_ms   REAL    DEFAULT -1,
            rssi_dbm    INTEGER DEFAULT -1000,
            tcp_loss    REAL    DEFAULT -1,
            quality     TEXT    DEFAULT '',
            score       REAL    DEFAULT 0,
            traffic_bps INTEGER DEFAULT 0,
            traffic_pps INTEGER DEFAULT 0,
            flows       INTEGER DEFAULT 0
        );
        CREATE INDEX IF NOT EXISTS idx_history_ts ON network_history(ts);
        CREATE INDEX IF NOT EXISTS idx_history_iface ON network_history(iface);
    )";
    return exec(schema);
}

bool DatabaseManager::insertSnapshot(const std::string& iface, const NetInfo& info, double score) {
    if (!db_) return false;

    std::lock_guard<std::mutex> lock(write_mutex_);

    // 使用参数绑定防止 SQL 注入
    const char* sql = "INSERT INTO network_history (ts, iface, rtt_ms, jitter_ms, rssi_dbm, "
                      "tcp_loss, quality, score, traffic_bps, traffic_pps, flows) VALUES ("
                      "?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        LOG_ERROR(LogModule::SYSTEM, "DatabaseManager::insertSnapshot prepare failed: " << sqlite3_errmsg(db_));
        return false;
    }

    std::string timestamp = currentTimestamp();
    std::string quality = qualityToString(info.quality());

    sqlite3_bind_text(stmt, 1, timestamp.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, iface.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, info.rttMs());
    sqlite3_bind_double(stmt, 4, info.jitterMs());
    sqlite3_bind_int(stmt, 5, info.rssiDbm());
    sqlite3_bind_double(stmt, 6, info.tcpLossRate());
    sqlite3_bind_text(stmt, 7, quality.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 8, score);
    sqlite3_bind_int64(stmt, 9, info.trafficTotalBps());
    sqlite3_bind_int64(stmt, 10, info.trafficTotalPps());
    sqlite3_bind_int(stmt, 11, info.trafficActiveFlows());

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        LOG_ERROR(LogModule::SYSTEM, "DatabaseManager::insertSnapshot step failed: " << sqlite3_errmsg(db_));
        return false;
    }

    return true;
}

std::string DatabaseManager::queryHistory(const std::string& interface,
                                           const std::string& start,
                                           const std::string& end,
                                                 int limit) {
    if (!db_) return "[]";

    // 使用参数绑定防止 SQL 注入
    std::string sql = "SELECT ts, iface, rtt_ms, jitter_ms, rssi_dbm, tcp_loss, quality, "
                      "score, traffic_bps, traffic_pps, flows "
                      "FROM network_history WHERE 1=1";

    std::vector<std::string> conditions;
    if (!interface.empty()) {
        sql += " AND iface=?";
        conditions.push_back(interface);
    }
    if (!start.empty()) {
        sql += " AND ts>=?";
        conditions.push_back(start);
    }
    if (!end.empty()) {
        sql += " AND ts<=?";
        conditions.push_back(end);
    }

    sql += " ORDER BY ts DESC LIMIT ?";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        LOG_ERROR(LogModule::SYSTEM, "DatabaseManager::queryHistory prepare failed: " << sqlite3_errmsg(db_));
        return "[]";
    }

    // 绑定参数
    int paramIndex = 1;
    for (const auto& cond : conditions) {
        sqlite3_bind_text(stmt, paramIndex++, cond.c_str(), -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_int(stmt, paramIndex, limit);

    HistoryCallbackCtx ctx;
    rc = sqlite3_step(stmt);
    while (rc == SQLITE_ROW) {
        HistoryRow row;
        const char* ts = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const char* iface = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        if (ts) row.ts = ts;
        if (iface) row.iface = iface;
        row.rtt_ms = sqlite3_column_int(stmt, 2);
        row.jitter_ms = sqlite3_column_double(stmt, 3);
        row.rssi_dbm = sqlite3_column_int(stmt, 4);
        row.tcp_loss = sqlite3_column_double(stmt, 5);
        const char* quality = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        if (quality) row.quality = quality;
        row.score = sqlite3_column_double(stmt, 7);
        row.traffic_bps = sqlite3_column_int64(stmt, 8);
        row.traffic_pps = sqlite3_column_int(stmt, 9);
        row.flows = sqlite3_column_int(stmt, 10);
        ctx.rows.push_back(std::move(row));
        rc = sqlite3_step(stmt);
    }

    sqlite3_finalize(stmt);

    // 构建 JSON 数组
    std::ostringstream json;
    json << "[";
    for (size_t i = 0; i < ctx.rows.size(); ++i) {
        const auto& row = ctx.rows[i];
        if (i > 0) json << ",";
        json << "{"
             << "\"ts\":\"" << weaknet_utils::escapeJsonString(row.ts) << "\","
             << "\"iface\":\"" << weaknet_utils::escapeJsonString(row.iface) << "\","
             << "\"rtt_ms\":" << row.rtt_ms << ","
             << "\"jitter_ms\":" << row.jitter_ms << ","
             << "\"rssi_dbm\":" << row.rssi_dbm << ","
             << "\"tcp_loss\":" << row.tcp_loss << ","
             << "\"quality\":\"" << weaknet_utils::escapeJsonString(row.quality) << "\","
             << "\"score\":" << row.score << ","
             << "\"traffic_bps\":" << row.traffic_bps << ","
             << "\"traffic_pps\":" << row.traffic_pps << ","
             << "\"flows\":" << row.flows
             << "}";
    }
    json << "]";

    return json.str();
}

int DatabaseManager::cleanup(int retention_days) {
    if (!db_) return 0;

    std::lock_guard<std::mutex> lock(write_mutex_);

    std::ostringstream sql;
    sql << "DELETE FROM network_history WHERE ts < datetime('now', '-"
        << retention_days << " days')";

    char* err = nullptr;
    int rc = sqlite3_exec(db_, sql.str().c_str(), nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        LOG_ERROR(LogModule::SYSTEM, "DatabaseManager::cleanup error: " << (err ? err : "unknown"));
        sqlite3_free(err);
        return 0;
    }

    int deleted = sqlite3_changes(db_);
    if (deleted > 0) {
        LOG_INFO(LogModule::SYSTEM, "DatabaseManager::cleanup deleted " << deleted << " rows older than " << retention_days << " days");
    }
    return deleted;
}

int64_t DatabaseManager::getRecordCount() {
    if (!db_) return 0;

    int64_t count = 0;
    sqlite3_exec(db_, "SELECT COUNT(*) FROM network_history", countCallback, &count, nullptr);
    return count;
}

static int stringRangeCallback(void* data, int /*argc*/, char** argv, char** /*colNames*/) {
    auto* s = static_cast<std::string*>(data);
    if (argv[0]) *s = argv[0];
    return 0;
}

std::string DatabaseManager::getDbInfo() {
    if (!db_) return "{\"error\":\"database not open\"}";

    int64_t count = getRecordCount();
    int64_t page_size = 0, page_count = 0;
    sqlite3_exec(db_, "PRAGMA page_size", countCallback, &page_size, nullptr);
    sqlite3_exec(db_, "PRAGMA page_count", countCallback, &page_count, nullptr);
    int64_t db_size_kb = (page_size * page_count) / 1024;

    // 获取时间范围
    std::string earliest, latest;
    sqlite3_exec(db_, "SELECT MIN(ts) FROM network_history", stringRangeCallback, &earliest, nullptr);
    sqlite3_exec(db_, "SELECT MAX(ts) FROM network_history", stringRangeCallback, &latest, nullptr);

    std::ostringstream info;
    info << "{"
         << "\"records\":" << count << ","
         << "\"size_kb\":" << db_size_kb << ","
         << "\"earliest\":\"" << earliest << "\","
         << "\"latest\":\"" << latest << "\""
         << "}";
    return info.str();
}

}  // namespace weaknet_dbus
