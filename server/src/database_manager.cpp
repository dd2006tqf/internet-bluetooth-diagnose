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
 *     仅由显式调用方（如 history_query_tool --cleanup）触发；服务端不会自动删除历史数据
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
#include "network_quality_assessor.hpp"
#include "utils/json_escape.hpp"
#include <sqlite3.h>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <array>
#include <map>
#include <sys/stat.h>

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
    std::string ts, iface, quality, link_quality, overall_quality, rssi_source;
    std::string rssi_status, rtt_status, jitter_status, tcp_loss_status, traffic_status;
    std::string snapshot_ts;
    int64_t generation = 0, data_version = 1;
    int64_t rtt_sample_ts = 0, rssi_sample_ts = 0, jitter_sample_ts = 0;
    int64_t tcp_loss_sample_ts = 0, traffic_sample_ts = 0;
    int rtt_ms = -1, rssi_dbm = -1000, traffic_pps = 0, flows = 0;
    bool rtt_null = false, jitter_null = false, rssi_null = false, tcp_loss_null = false;
    bool traffic_null = false;
    bool rssi_estimated = false;
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
    if (argv[5]) row.rssi_source = argv[5];
    if (argv[6]) row.rssi_estimated = atoi(argv[6]) != 0;
    if (argv[7]) row.rssi_status = argv[7];
    if (argv[8]) row.rtt_status = argv[8];
    if (argv[9]) row.jitter_status = argv[9];
    if (argv[10]) row.tcp_loss_status = argv[10];
    if (argv[11]) row.traffic_status = argv[11];
    if (argv[12]) row.tcp_loss = atof(argv[12]);
    if (argv[13]) row.quality = argv[13];
    if (argv[14]) row.link_quality = argv[14];
    if (argv[15]) row.overall_quality = argv[15];
    if (argv[16]) row.score = atof(argv[16]);
    if (argv[18]) row.traffic_bps = atoll(argv[18]);
    if (argv[19]) row.traffic_pps = atoi(argv[19]);
    if (argv[20]) row.flows = atoi(argv[20]);
    if (argv[21]) row.snapshot_ts = argv[21];
    if (argv[22]) row.generation = atoll(argv[22]);
    if (argv[23]) row.data_version = atoll(argv[23]);
    if (argv[24]) row.rtt_sample_ts = atoll(argv[24]);
    if (argv[25]) row.rssi_sample_ts = atoll(argv[25]);
    if (argv[26]) row.jitter_sample_ts = atoll(argv[26]);
    if (argv[27]) row.tcp_loss_sample_ts = atoll(argv[27]);
    if (argv[28]) row.traffic_sample_ts = atoll(argv[28]);
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
        } else {
            // 收敛目录权限（默认 0755 太宽；服务端以 root 运行，0750 阻止其它用户读网络快照历史）
            ::chmod(dir.c_str(), 0750);
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
            rssi_source TEXT DEFAULT '',
            rssi_estimated INTEGER DEFAULT 0,
            rssi_status TEXT DEFAULT 'unavailable',
            rtt_status TEXT DEFAULT 'unavailable',
            jitter_status TEXT DEFAULT 'unavailable',
            tcp_loss_status TEXT DEFAULT 'unavailable',
            traffic_status TEXT DEFAULT 'unavailable',
            tcp_loss    REAL    DEFAULT -1,
            quality     TEXT    DEFAULT '',
            link_quality TEXT   DEFAULT '',
            overall_quality TEXT DEFAULT '',
            overall_score REAL  DEFAULT 0,
            score       REAL    DEFAULT 0,
            traffic_bps INTEGER DEFAULT 0,
            traffic_pps INTEGER DEFAULT 0,
            flows       INTEGER DEFAULT 0,
            snapshot_ts TEXT    DEFAULT '',
            generation  INTEGER DEFAULT 0,
            data_version INTEGER DEFAULT 1,
            rtt_sample_ts INTEGER DEFAULT 0,
            rssi_sample_ts INTEGER DEFAULT 0,
            jitter_sample_ts INTEGER DEFAULT 0,
            tcp_loss_sample_ts INTEGER DEFAULT 0,
            traffic_sample_ts INTEGER DEFAULT 0
        );
        CREATE INDEX IF NOT EXISTS idx_history_ts ON network_history(ts);
        CREATE INDEX IF NOT EXISTS idx_history_iface ON network_history(iface);
    )";
    if (!exec(schema)) return false;

    auto ensureColumn = [this](const char* name, const char* definition) {
        sqlite3_stmt* stmt = nullptr;
        const char* query = "SELECT 1 FROM pragma_table_info('network_history') WHERE name=?";
        if (sqlite3_prepare_v2(db_, query, -1, &stmt, nullptr) != SQLITE_OK) return false;
        sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
        const bool exists = sqlite3_step(stmt) == SQLITE_ROW;
        sqlite3_finalize(stmt);
        if (exists) return true;
        return exec(std::string("ALTER TABLE network_history ADD COLUMN ") + name + " " + definition);
    };
    return ensureColumn("rssi_source", "TEXT DEFAULT ''") &&
           ensureColumn("rssi_estimated", "INTEGER DEFAULT 0") &&
           ensureColumn("rssi_status", "TEXT DEFAULT 'unavailable'") &&
           ensureColumn("rtt_status", "TEXT DEFAULT 'unavailable'") &&
           ensureColumn("jitter_status", "TEXT DEFAULT 'unavailable'") &&
           ensureColumn("tcp_loss_status", "TEXT DEFAULT 'unavailable'") &&
           ensureColumn("traffic_status", "TEXT DEFAULT 'unavailable'") &&
           ensureColumn("link_quality", "TEXT DEFAULT ''") &&
           ensureColumn("overall_quality", "TEXT DEFAULT ''") &&
           ensureColumn("overall_score", "REAL DEFAULT 0") &&
           ensureColumn("snapshot_ts", "TEXT DEFAULT ''") &&
           ensureColumn("generation", "INTEGER DEFAULT 0") &&
           ensureColumn("data_version", "INTEGER DEFAULT 1") &&
           ensureColumn("rtt_sample_ts", "INTEGER DEFAULT 0") &&
           ensureColumn("rssi_sample_ts", "INTEGER DEFAULT 0") &&
           ensureColumn("jitter_sample_ts", "INTEGER DEFAULT 0") &&
           ensureColumn("tcp_loss_sample_ts", "INTEGER DEFAULT 0") &&
           ensureColumn("traffic_sample_ts", "INTEGER DEFAULT 0");
}

bool DatabaseManager::insertSnapshot(const std::string& iface, const NetInfo& info,
                                      const NetworkQualityResult& overall,
                                      uint64_t generation, int64_t snapshot_ts_ms,
                                      int64_t rtt_sample_ts, int64_t rssi_sample_ts,
                                      int64_t jitter_sample_ts, int64_t tcp_loss_sample_ts,
                                      int64_t traffic_sample_ts) {
    if (!db_) return false;

    std::lock_guard<std::mutex> lock(mutex_);

    // 使用参数绑定防止 SQL 注入
    const char* sql = "INSERT INTO network_history (ts, iface, rtt_ms, jitter_ms, rssi_dbm, rssi_source, rssi_estimated, "
                      "rssi_status, rtt_status, jitter_status, tcp_loss_status, traffic_status, tcp_loss, quality, link_quality, overall_quality, overall_score, score, traffic_bps, traffic_pps, flows, snapshot_ts, generation, data_version, rtt_sample_ts, rssi_sample_ts, jitter_sample_ts, tcp_loss_sample_ts, traffic_sample_ts) VALUES ("
                      "?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ? )";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        LOG_ERROR(LogModule::SYSTEM, "DatabaseManager::insertSnapshot prepare failed: " << sqlite3_errmsg(db_));
        return false;
    }

    std::string timestamp = currentTimestamp();

    // stale 语义：以快照时间为基准，指标采样时间缺失(<=0)或超过 30 秒视为 stale；
    // 陈旧指标值写 NULL，状态写 stale，避免把旧值当作当前采样。
    const int64_t kStaleWindowMs = 30000;
    const int64_t now = snapshot_ts_ms > 0 ? snapshot_ts_ms
        : std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    auto stale = [&](int64_t sample_ts) {
        return sample_ts <= 0 || now < sample_ts || now - sample_ts > kStaleWindowMs;
    };
    const bool rttStale = stale(rtt_sample_ts);
    const bool rssiStale = stale(rssi_sample_ts);
    const bool jitterStale = stale(jitter_sample_ts);
    const bool tcpLossStale = stale(tcp_loss_sample_ts);
    const bool trafficStale = stale(traffic_sample_ts);

    std::string quality = overall.levelName;
    if (quality.empty()) quality = NetworkQualityAssessor::getQualityLevelName(overall.level);

    sqlite3_bind_text(stmt, 1, timestamp.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, iface.c_str(), -1, SQLITE_TRANSIENT);
    if (info.rttMs() >= 0 && !rttStale) sqlite3_bind_int(stmt, 3, info.rttMs());
    else sqlite3_bind_null(stmt, 3);
    if (info.jitterMs() >= 0 && !jitterStale) sqlite3_bind_double(stmt, 4, info.jitterMs());
    else sqlite3_bind_null(stmt, 4);
    const int rssiDbm = info.rssiDbm();
    const bool rssiValid = !rssiStale && rssiDbm >= -100 && rssiDbm <= 0 &&
        (!info.rssiEstimated() || rssiDbm <= -30);
    if (rssiValid) sqlite3_bind_int(stmt, 5, rssiDbm);
    else sqlite3_bind_null(stmt, 5);
    const std::string rssiSource = info.rssiSource();
    const std::string rssiStatus = rssiStale ? "stale" : (rssiValid ? "valid" : "unavailable");
    const std::string rttStatus = rttStale ? "stale"
        : (info.rttMs() >= 0 ? "valid" : (info.rttMs() == -5 ? "timeout" : "unavailable"));
    const std::string jitterStatus = jitterStale ? "stale"
        : (info.jitterMs() >= 0 ? "valid" : "unavailable");
    const std::string tcpLossStatus = tcpLossStale ? "stale"
        : (info.tcpLossRate() >= 0 ? "valid" : "unavailable");
    const std::string trafficStatus = trafficStale ? "stale"
        : (info.trafficTotalBps() || info.trafficTotalPps() || info.trafficActiveFlows() ? "valid" : "unavailable");
    sqlite3_bind_text(stmt, 6, rssiSource.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 7, info.rssiEstimated() ? 1 : 0);
    sqlite3_bind_text(stmt, 8, rssiStatus.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 9, rttStatus.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 10, jitterStatus.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 11, tcpLossStatus.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 12, trafficStatus.c_str(), -1, SQLITE_TRANSIENT);
    if (info.tcpLossRate() >= 0 && !tcpLossStale) sqlite3_bind_double(stmt, 13, info.tcpLossRate());
    else sqlite3_bind_null(stmt, 13);
    sqlite3_bind_text(stmt, 14, quality.c_str(), -1, SQLITE_TRANSIENT);
    const std::string linkQuality = qualityToString(info.quality());
    sqlite3_bind_text(stmt, 15, linkQuality.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 16, quality.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 17, overall.score);
    sqlite3_bind_double(stmt, 18, overall.score);
    if (!trafficStale) {
        sqlite3_bind_int64(stmt, 19, info.trafficTotalBps());
        sqlite3_bind_int64(stmt, 20, info.trafficTotalPps());
        sqlite3_bind_int(stmt, 21, info.trafficActiveFlows());
    } else {
        sqlite3_bind_null(stmt, 19);
        sqlite3_bind_null(stmt, 20);
        sqlite3_bind_null(stmt, 21);
    }
    const std::string snapshotTimestamp = snapshot_ts_ms > 0 ? std::to_string(snapshot_ts_ms) : timestamp;
    sqlite3_bind_text(stmt, 22, snapshotTimestamp.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 23, static_cast<sqlite3_int64>(generation));
    sqlite3_bind_int(stmt, 24, 2);
    sqlite3_bind_int64(stmt, 25, rtt_sample_ts);
    sqlite3_bind_int64(stmt, 26, rssi_sample_ts);
    sqlite3_bind_int64(stmt, 27, jitter_sample_ts);
    sqlite3_bind_int64(stmt, 28, tcp_loss_sample_ts);
    sqlite3_bind_int64(stmt, 29, traffic_sample_ts);
    LOG_INFO(LogModule::SYSTEM, "snapshot metadata: generation=" << generation
             << " snapshot_ts=" << snapshot_ts_ms
             << " rtt=" << rtt_sample_ts << " rssi=" << rssi_sample_ts
             << " jitter=" << jitter_sample_ts << " tcp_loss=" << tcp_loss_sample_ts
             << " traffic=" << traffic_sample_ts);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        LOG_ERROR(LogModule::SYSTEM, "DatabaseManager::insertSnapshot step failed: " << sqlite3_errmsg(db_));
        return false;
    }

    return true;
}

bool DatabaseManager::insertSnapshot(const std::string& iface, const NetInfo& info, double score) {
    NetworkQualityResult overall;
    overall.score = score;
    overall.level = NetworkQualityLevel::UNKNOWN;
    overall.levelName = "UNKNOWN";
    return insertSnapshot(iface, info, overall,
                           info.generation(), info.lastUpdatedMs(),
                           info.rttSampleTsMs(), info.rssiSampleTsMs(),
                           info.jitterSampleTsMs(), info.tcpLossSampleTsMs(),
                           info.trafficSampleTsMs());
}

std::string DatabaseManager::queryHistory(const std::string& interface,
                                           const std::string& start,
                                           const std::string& end,
                                                 int limit) {
    if (!db_) return "[]";
    std::lock_guard<std::mutex> lock(mutex_);

    // 使用参数绑定防止 SQL 注入
    std::string sql = "SELECT ts, iface, rtt_ms, jitter_ms, rssi_dbm, rssi_source, rssi_estimated, rssi_status, rtt_status, jitter_status, tcp_loss_status, traffic_status, tcp_loss, quality, link_quality, overall_quality, overall_score, score, traffic_bps, traffic_pps, flows, snapshot_ts, generation, data_version, rtt_sample_ts, rssi_sample_ts, jitter_sample_ts, tcp_loss_sample_ts, traffic_sample_ts "
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
        row.rtt_null = sqlite3_column_type(stmt, 2) == SQLITE_NULL;
        row.rtt_ms = row.rtt_null ? -1 : sqlite3_column_int(stmt, 2);
        row.jitter_null = sqlite3_column_type(stmt, 3) == SQLITE_NULL;
        row.jitter_ms = row.jitter_null ? -1 : sqlite3_column_double(stmt, 3);
        row.rssi_null = sqlite3_column_type(stmt, 4) == SQLITE_NULL;
        row.rssi_dbm = row.rssi_null ? -1000 : sqlite3_column_int(stmt, 4);
        const char* rssi_source = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        if (rssi_source) row.rssi_source = rssi_source;
        row.rssi_estimated = sqlite3_column_int(stmt, 6) != 0;
        const char* rssi_status = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
        if (rssi_status) row.rssi_status = rssi_status;
        const char* rtt_status = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));
        if (rtt_status) row.rtt_status = rtt_status;
        const char* jitter_status = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));
        if (jitter_status) row.jitter_status = jitter_status;
        const char* tcp_loss_status = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 10));
        if (tcp_loss_status) row.tcp_loss_status = tcp_loss_status;
        const char* traffic_status = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 11));
        if (traffic_status) row.traffic_status = traffic_status;
        row.tcp_loss_null = sqlite3_column_type(stmt, 12) == SQLITE_NULL;
        row.tcp_loss = row.tcp_loss_null ? -1 : sqlite3_column_double(stmt, 12);
        const char* quality = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 13));
        if (quality) row.quality = quality;
        row.score = sqlite3_column_double(stmt, 16);
        const char* link_quality = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 14));
        if (link_quality) row.link_quality = link_quality;
        const char* overall_quality = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 15));
        if (overall_quality) row.overall_quality = overall_quality;
        row.traffic_null = sqlite3_column_type(stmt, 18) == SQLITE_NULL;
        row.traffic_bps = row.traffic_null ? 0 : sqlite3_column_int64(stmt, 18);
        row.traffic_pps = sqlite3_column_int(stmt, 19);
        row.flows = sqlite3_column_int(stmt, 20);
        const char* snapshot_ts = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 21));
        if (snapshot_ts) row.snapshot_ts = snapshot_ts;
        row.generation = sqlite3_column_int64(stmt, 22);
        row.data_version = sqlite3_column_int64(stmt, 23);
        row.rtt_sample_ts = sqlite3_column_int64(stmt, 24);
        row.rssi_sample_ts = sqlite3_column_int64(stmt, 25);
        row.jitter_sample_ts = sqlite3_column_int64(stmt, 26);
        row.tcp_loss_sample_ts = sqlite3_column_int64(stmt, 27);
        row.traffic_sample_ts = sqlite3_column_int64(stmt, 28);
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
        std::string rssiAge = "null";
        try {
            const int64_t snapshotMs = std::stoll(row.snapshot_ts);
            if (snapshotMs >= row.rssi_sample_ts && row.rssi_sample_ts > 0) {
                rssiAge = std::to_string(snapshotMs - row.rssi_sample_ts);
            }
        } catch (const std::exception&) {
            // Legacy rows use a formatted timestamp and have no reliable age.
        }
        json << "{"
             << "\"ts\":\"" << weaknet_utils::escapeJsonString(row.ts) << "\","
             << "\"iface\":\"" << weaknet_utils::escapeJsonString(row.iface) << "\","
             << "\"rtt_ms\":" << (row.rtt_null ? "null" : std::to_string(row.rtt_ms)) << ","
             << "\"jitter_ms\":" << (row.jitter_null ? "null" : std::to_string(row.jitter_ms)) << ","
             << "\"rssi_dbm\":" << (row.rssi_null ? "null" : std::to_string(row.rssi_dbm)) << ","
             << "\"rssi_source\":\"" << weaknet_utils::escapeJsonString(row.rssi_source) << "\","
             << "\"rssi_estimated\":" << (row.rssi_estimated ? "true" : "false") << ","
             << "\"rssi_status\":\"" << weaknet_utils::escapeJsonString(row.rssi_status) << "\","
             << "\"rtt_status\":\"" << weaknet_utils::escapeJsonString(row.rtt_status) << "\","
             << "\"jitter_status\":\"" << weaknet_utils::escapeJsonString(row.jitter_status) << "\","
             << "\"tcp_loss_status\":\"" << weaknet_utils::escapeJsonString(row.tcp_loss_status) << "\","
             << "\"traffic_status\":\"" << weaknet_utils::escapeJsonString(row.traffic_status) << "\","
             << "\"tcp_loss\":" << (row.tcp_loss_null ? "null" : std::to_string(row.tcp_loss)) << ","
             << "\"quality\":\"" << weaknet_utils::escapeJsonString(row.quality) << "\","
             << "\"link_quality\":\"" << weaknet_utils::escapeJsonString(row.link_quality) << "\","
             << "\"overall_quality\":\"" << weaknet_utils::escapeJsonString(row.overall_quality) << "\","
             << "\"overall_score\":" << std::fixed << std::setprecision(6) << row.score << ","
             << "\"score\":" << std::fixed << std::setprecision(6) << row.score << ","
             << "\"traffic_bps\":" << (row.traffic_null ? "null" : std::to_string(row.traffic_bps)) << ","
             << "\"traffic_pps\":" << (row.traffic_null ? "null" : std::to_string(row.traffic_pps)) << ","
             << "\"flows\":" << (row.traffic_null ? "null" : std::to_string(row.flows)) << ","
             << "\"snapshot_ts\":\"" << weaknet_utils::escapeJsonString(row.snapshot_ts) << "\","
             << "\"generation\":" << row.generation << ","
             << "\"data_version\":" << row.data_version << ","
             << "\"legacy\":" << (row.data_version < 2 ? "true" : "false") << ","
             << "\"rtt_sample_ts\":" << row.rtt_sample_ts << ","
             << "\"rssi_sample_ts\":" << row.rssi_sample_ts << ","
             << "\"jitter_sample_ts\":" << row.jitter_sample_ts << ","
             << "\"tcp_loss_sample_ts\":" << row.tcp_loss_sample_ts << ","
             << "\"traffic_sample_ts\":" << row.traffic_sample_ts << ","
             << "\"rssi_age_ms\":" << rssiAge
             << "}";
    }
    json << "]";

    return json.str();
}

std::string DatabaseManager::getQualityReport() {
    if (!db_) return "{\"error\":\"database not open\"}";
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT iface, rssi_status, rtt_status, jitter_status, tcp_loss_status, traffic_status, rssi_estimated, data_version, rtt_sample_ts, rssi_sample_ts, jitter_sample_ts, tcp_loss_sample_ts, traffic_sample_ts FROM network_history ORDER BY iface";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return "{\"error\":\"quality report query failed\"}";
    struct Stats { int64_t total = 0, legacy = 0, estimated = 0; std::map<std::string, int64_t> status[5]; int64_t zero[5] = {}; };
    std::map<std::string, Stats> interfaces;
    int64_t total = 0, legacy = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (!name) continue;
        auto& stats = interfaces[name];
        ++stats.total; ++total;
        if (sqlite3_column_int64(stmt, 7) < 2) { ++stats.legacy; ++legacy; }
        if (sqlite3_column_int(stmt, 6) != 0) ++stats.estimated;
        for (int i = 0; i < 5; ++i) {
            const char* status = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1 + i));
            ++stats.status[i][status && *status ? status : "unavailable"];
            if (sqlite3_column_int64(stmt, 8 + i) == 0) ++stats.zero[i];
        }
    }
    sqlite3_finalize(stmt);
    auto statuses = [](const std::map<std::string, int64_t>& counts) {
        std::ostringstream out; out << "{"; bool first = true;
        for (const auto& item : counts) { if (!first) out << ","; first = false; out << "\"" << item.first << "\":" << item.second; }
        out << "}"; return out.str();
    };
    std::ostringstream json;
    json << "{\"total\":" << total << ",\"legacy_records\":" << legacy
         << ",\"legacy_ratio\":" << (total ? static_cast<double>(legacy) / total : 0.0)
         << ",\"interfaces\":{";
    bool first = true;
    for (const auto& item : interfaces) {
        if (!first) json << ",";
        first = false;
        const auto& name = item.first; const auto& s = item.second;
        json << "\"" << weaknet_utils::escapeJsonString(name) << "\":{\"total\":" << s.total
             << ",\"legacy_records\":" << s.legacy
             << ",\"rssi_estimated_count\":" << s.estimated
             << ",\"rssi_estimated_ratio\":" << (s.total ? static_cast<double>(s.estimated) / s.total : 0.0)
             << ",\"status_counts\":{";
        json << "\"rssi\":" << statuses(s.status[0]) << ",\"rtt\":" << statuses(s.status[1])
             << ",\"jitter\":" << statuses(s.status[2]) << ",\"tcp_loss\":" << statuses(s.status[3])
             << ",\"traffic\":" << statuses(s.status[4]) << "},\"timestamp_zero_counts\":{";
        json << "\"rtt\":" << s.zero[1] << ",\"rssi\":" << s.zero[0]
             << ",\"jitter\":" << s.zero[2] << ",\"tcp_loss\":" << s.zero[3]
             << ",\"traffic\":" << s.zero[4] << "}}";
    }
    json << "}}";
    return json.str();
}
int DatabaseManager::cleanup(int retention_days) {
    if (!db_) return -1;

    std::lock_guard<std::mutex> lock(mutex_);

    std::ostringstream sql;
    sql << "DELETE FROM network_history WHERE ts < datetime('now', '-"
        << retention_days << " days')";

    char* err = nullptr;
    int rc = sqlite3_exec(db_, sql.str().c_str(), nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        LOG_ERROR(LogModule::SYSTEM, "DatabaseManager::cleanup error: " << (err ? err : "unknown"));
        sqlite3_free(err);
        return -1;
    }

    int deleted = sqlite3_changes(db_);
    if (deleted > 0) {
        LOG_INFO(LogModule::SYSTEM, "DatabaseManager::cleanup deleted " << deleted << " rows older than " << retention_days << " days");
    }
    return deleted;
}

int64_t DatabaseManager::getRecordCountLocked() {
    int64_t count = 0;
    sqlite3_exec(db_, "SELECT COUNT(*) FROM network_history", countCallback, &count, nullptr);
    return count;
}

int64_t DatabaseManager::getRecordCount() {
    if (!db_) return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    return getRecordCountLocked();
}

static int stringRangeCallback(void* data, int /*argc*/, char** argv, char** /*colNames*/) {
    auto* s = static_cast<std::string*>(data);
    if (argv[0]) *s = argv[0];
    return 0;
}

std::string DatabaseManager::getDbInfo() {
    if (!db_) return "{\"error\":\"database not open\"}";
    std::lock_guard<std::mutex> lock(mutex_);

    int64_t count = getRecordCountLocked();
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
