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
#include "network_quality_result.hpp"
#include "site_incident.hpp"
#include "utils/json_escape.hpp"
#include "wireless_event.hpp"
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

// 使用预定义列名的结构
struct HistoryRow {
    std::string ts, iface, quality, link_quality, overall_quality, rssi_source;
    std::string rssi_status, rtt_status, jitter_status, tcp_loss_status, traffic_status;
    std::string snapshot_ts, score_model, assessment_profile;
    int64_t generation = 0, data_version = 1;
    int64_t rtt_sample_ts = 0, rssi_sample_ts = 0, jitter_sample_ts = 0;
    int64_t tcp_loss_sample_ts = 0, traffic_sample_ts = 0;
    int rtt_ms = -1, rssi_dbm = -1000;
    int64_t traffic_pps = 0, flows = 0;
    bool rtt_null = false, jitter_null = false, rssi_null = false, tcp_loss_null = false;
    bool traffic_null = false;
    bool rssi_estimated = false;
    double jitter_ms = -1, tcp_loss = -1, score = 0;
    int64_t traffic_bps = 0;
};

struct HistoryCallbackCtx {
    std::vector<HistoryRow> rows;
};

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

        CREATE TABLE IF NOT EXISTS bt_history (
            id                 INTEGER PRIMARY KEY AUTOINCREMENT,
            ts                 TEXT    NOT NULL,
            adapter_mac        TEXT    DEFAULT '',
            device_mac         TEXT    NOT NULL,
            device_name        TEXT    DEFAULT '',
            connected          INTEGER DEFAULT 0,
            rssi_dbm           REAL    DEFAULT -1000,
            distance_m         REAL    DEFAULT -1.0,
            audio_active       INTEGER DEFAULT 0,
            quality_score      REAL    DEFAULT 0,
            suspected_stall    INTEGER DEFAULT 0,
            bytes_per_sec      INTEGER DEFAULT 0,
            max_gap_ms         INTEGER DEFAULT 0
        );
        CREATE INDEX IF NOT EXISTS idx_bt_history_ts ON bt_history(ts);
        CREATE INDEX IF NOT EXISTS idx_bt_history_dev ON bt_history(device_mac);

        -- device_events: 规范化无线设备事件（Phase 1: Bluetooth Device Event）
        --
        -- 与 bt_history 的语义区别：bt_history 是"周期采样快照"（每 N 秒一行，
        -- 描述设备当时的状态），本表是"事件"（只在发生断连/发现/劣化时写一行）。
        -- 两者并存，互不替代。
        --
        -- 列设计要点：
        --   site_id / gateway_id  事件来源身份（"这个事件是哪台探针看到的"），
        --                         Phase 1 为常量，但必须现在就进 schema——
        --                         否则多网关出现后无法回答历史事件归属。
        --   rssi_at_event_dbm     NULL 表示未采集，**不是 0**。0 会污染后续 RSSI 统计。
        --   raw_reason_code       内核原始 HCI reason，无损保存
        --   reason                归一化解释；与 raw_reason_code 同时保存，
        --                         前者不因后者而丢弃（Other 不能成为信息黑洞）
        --   suspected_cause       推断原因。Phase 1 恒为 NULL——采集层不做诊断。
        --   details_json          仅存审计性证据（raw_evidence 数组）
        CREATE TABLE IF NOT EXISTS device_events (
            id                 INTEGER PRIMARY KEY AUTOINCREMENT,
            event_id           TEXT    UNIQUE,
            ts                 INTEGER NOT NULL,
            site_id            TEXT    DEFAULT '',
            gateway_id         TEXT    DEFAULT '',
            protocol           TEXT    DEFAULT 'BLUETOOTH',
            device_address     TEXT    NOT NULL,
            address_type       TEXT    DEFAULT 'UNKNOWN',
            hci_index          INTEGER DEFAULT 0,
            event_type         TEXT    NOT NULL,
            rssi_at_event_dbm  INTEGER DEFAULT NULL,
            raw_reason_code    INTEGER DEFAULT 0,
            reason             TEXT    DEFAULT 'UNKNOWN',
            source             TEXT    DEFAULT 'UNKNOWN',
            source_detail      TEXT    DEFAULT '',
            suspected_cause    TEXT    DEFAULT NULL,
            details_json       TEXT    DEFAULT ''
        );
        CREATE INDEX IF NOT EXISTS idx_device_events_ts ON device_events(ts);
        CREATE INDEX IF NOT EXISTS idx_device_events_dev ON device_events(device_address);

        -- device_baselines: 链路基线与轻量画像（Phase 2: 复合主键，UPSERT 策略）
        --
        -- 核心设计准则：
        --   1. 复合主键：(site_id, gateway_id, hci_index, protocol, address_type, device_address)
        --      杜绝跨网关、跨控制器、跨地址类型（BREDR vs LE Public vs LE Random）污染。
        --   2. 去除派生统计：不存 degraded_count/disconnect_count，统计以 device_events 为单一真值。
        --   3. 可信基线：baseline_rssi_dbm 为当前稳定的滑动中位数基线；
        --      处于 DEGRADED 期间基线冻结，不更新入库；未收敛或初始为 NULL。
        CREATE TABLE IF NOT EXISTS device_baselines (
            site_id             TEXT    NOT NULL,
            gateway_id          TEXT    NOT NULL,
            hci_index           INTEGER NOT NULL,
            protocol            TEXT    NOT NULL,
            address_type        TEXT    NOT NULL,
            device_address      TEXT    NOT NULL,
            baseline_rssi_dbm   INTEGER DEFAULT NULL,
            min_seen_rssi_dbm   INTEGER DEFAULT NULL,
            max_seen_rssi_dbm   INTEGER DEFAULT NULL,
            baseline_sample_count INTEGER DEFAULT 0,
            first_seen_ms       INTEGER NOT NULL,
            last_seen_ms        INTEGER NOT NULL,
            state               TEXT    DEFAULT 'LEARNING',
            updated_at          INTEGER NOT NULL,
            PRIMARY KEY (site_id, gateway_id, hci_index, protocol, address_type, device_address)
        );
        CREATE INDEX IF NOT EXISTS idx_device_baselines_updated ON device_baselines(updated_at);

        -- site_incidents: 区域级异常事件（Phase 3a: SiteIncident）
        --
        -- Event 是不可变事实，Incident 是持续解释，两者生命周期不同，必须分开建模。
        -- 一行 = "该现场在这一时段内有多台设备同时出现合格异常"这一解释，
        -- 而不是某一条事件的副本。
        --
        -- 列设计要点：
        --   incident_id       确定性 ID = <prefix>_<site>_<started_at_ms>。
        --                     刻意不带进程实例随机码——服务重启后的回放会重新推导出
        --                     同一个 incident_id，必须写回同一行（UPSERT）。
        --                     对比 device_events.event_id 走的是相反策略（那里必须带
        --                     实例随机码，否则跨重启撞 UNIQUE 被 INSERT OR IGNORE 静默吞掉）。
        --   started_at_ms     覆盖的最早合格异常时刻（不是"发现时刻"）
        --   last_event_ms     最近吸收的合格异常时刻；时间过滤以此列为准
        --   resolved_at_ms    NULL 表示仍在活跃，**不是 0**（0 会造出"1970 年结案"）
        --   affected_devices  受影响设备**数量**（事实）。清单由回链表派生，
        --                     不在此重复存储，避免同一事实有两个来源。
        --   suspected_cause   推断原因。Phase 3a 恒为 NULL——关联层只做时空聚合。
        CREATE TABLE IF NOT EXISTS site_incidents (
            incident_id        TEXT    PRIMARY KEY,
            site_id            TEXT    DEFAULT '',
            gateway_id         TEXT    DEFAULT '',
            started_at_ms      INTEGER NOT NULL,
            last_event_ms      INTEGER NOT NULL,
            resolved_at_ms     INTEGER DEFAULT NULL,
            affected_devices   INTEGER DEFAULT 0,
            state              TEXT    DEFAULT 'OPEN',
            suspected_cause    TEXT    DEFAULT NULL
        );
        CREATE INDEX IF NOT EXISTS idx_site_incidents_last_event ON site_incidents(last_event_ms);
        CREATE INDEX IF NOT EXISTS idx_site_incidents_state ON site_incidents(state);

        -- site_incident_events: incident 与其证据事件的回链（1:N）
        --
        -- 存在的根本原因：**可追溯性**。用户问"凌晨 3 点影响了哪些设备"时，
        -- 答案必须是"这几条具体事件"，每条都能追到 device_events.event_id。
        -- 把一批 event_id 拼成文本塞进 incident 行做不到按 ID 精确回溯，
        -- 也无法在回放/重试时保证不重复计数。
        --
        -- 复合主键上的 INSERT OR IGNORE 使回放与重试天然幂等。
        -- 不建 FOREIGN KEY：device_events 有保留期清理（cleanup），
        -- 强制外键会让清理失败或级联删除事故证据；调用方通过 event_id 关联查询。
        CREATE TABLE IF NOT EXISTS site_incident_events (
            incident_id        TEXT    NOT NULL,
            event_id           TEXT    NOT NULL,
            PRIMARY KEY (incident_id, event_id)
        );
        CREATE INDEX IF NOT EXISTS idx_site_incident_events_event ON site_incident_events(event_id);
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
           ensureColumn("traffic_sample_ts", "INTEGER DEFAULT 0") &&
           ensureColumn("score_model", "TEXT DEFAULT 'legacy_weighted_v1'") &&
           ensureColumn("assessment_profile", "TEXT DEFAULT 'UNSPECIFIED'");
}

bool DatabaseManager::insertSnapshot(const std::string& iface, const NetInfo& info,
                                      const NetworkQualityResult& overall,
                                      uint64_t generation, int64_t snapshot_ts_ms,
                                      int64_t rtt_sample_ts, int64_t rssi_sample_ts,
                                      int64_t jitter_sample_ts, int64_t tcp_loss_sample_ts,
                                      int64_t traffic_sample_ts,
                                      const char* assessment_profile) {
    if (!db_) return false;

    std::lock_guard<std::mutex> lock(mutex_);

    // 使用参数绑定防止 SQL 注入
    const char* sql = "INSERT INTO network_history (ts, iface, rtt_ms, jitter_ms, rssi_dbm, rssi_source, rssi_estimated, "
                      "rssi_status, rtt_status, jitter_status, tcp_loss_status, traffic_status, tcp_loss, quality, link_quality, overall_quality, overall_score, score, traffic_bps, traffic_pps, flows, snapshot_ts, generation, data_version, rtt_sample_ts, rssi_sample_ts, jitter_sample_ts, tcp_loss_sample_ts, traffic_sample_ts, score_model, assessment_profile) VALUES ("
                      "?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ? )";

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
    if (quality.empty()) {
        switch (overall.level) {
            case NetworkQualityLevel::EXCELLENT: quality = "EXCELLENT"; break;
            case NetworkQualityLevel::GOOD: quality = "GOOD"; break;
            case NetworkQualityLevel::FAIR: quality = "FAIR"; break;
            case NetworkQualityLevel::POOR: quality = "POOR"; break;
            case NetworkQualityLevel::UNKNOWN:
            default: quality = "UNKNOWN"; break;
        }
    }

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
    // HR-9 & SR-6: 评分语义版本 assurance_v2；assessment_profile 取调用方传入的
    // 真实评估 Profile，不再写死 INTERNET_ACCESS。
    sqlite3_bind_text(stmt, 30, "assurance_v2", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 31,
                      (assessment_profile && *assessment_profile) ? assessment_profile : "UNSPECIFIED",
                      -1, SQLITE_TRANSIENT);
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

std::string DatabaseManager::queryHistory(const std::string& interface,
                                           const std::string& start,
                                           const std::string& end,
                                                 int limit) {
    if (!db_) return "[]";
    std::lock_guard<std::mutex> lock(mutex_);

    // 使用参数绑定防止 SQL 注入
    std::string sql = "SELECT ts, iface, rtt_ms, jitter_ms, rssi_dbm, rssi_source, rssi_estimated, rssi_status, rtt_status, jitter_status, tcp_loss_status, traffic_status, tcp_loss, quality, link_quality, overall_quality, overall_score, score, traffic_bps, traffic_pps, flows, snapshot_ts, generation, data_version, rtt_sample_ts, rssi_sample_ts, jitter_sample_ts, tcp_loss_sample_ts, traffic_sample_ts, score_model, assessment_profile "
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
        row.traffic_pps = sqlite3_column_int64(stmt, 19);
        row.flows = sqlite3_column_int64(stmt, 20);
        const char* snapshot_ts = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 21));
        if (snapshot_ts) row.snapshot_ts = snapshot_ts;
        row.generation = sqlite3_column_int64(stmt, 22);
        row.data_version = sqlite3_column_int64(stmt, 23);
        row.rtt_sample_ts = sqlite3_column_int64(stmt, 24);
        row.rssi_sample_ts = sqlite3_column_int64(stmt, 25);
        row.jitter_sample_ts = sqlite3_column_int64(stmt, 26);
        row.tcp_loss_sample_ts = sqlite3_column_int64(stmt, 27);
        row.traffic_sample_ts = sqlite3_column_int64(stmt, 28);
        const char* sm = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 29));
        if (sm) row.score_model = sm;
        const char* ap = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 30));
        if (ap) row.assessment_profile = ap;
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
            if (!row.snapshot_ts.empty()) {
                const int64_t snapshotMs = std::stoll(row.snapshot_ts);
                if (snapshotMs >= row.rssi_sample_ts && row.rssi_sample_ts > 0) {
                    rssiAge = std::to_string(snapshotMs - row.rssi_sample_ts);
                }
            }
        } catch (const std::exception& ex) {
            LOG_DEBUG(LogModule::SYSTEM, "row.snapshot_ts parse failed (" << row.snapshot_ts << "): " << ex.what());
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
             << "\"score_model\":\"" << weaknet_utils::escapeJsonString(row.score_model.empty() ? "legacy_weighted_v1" : row.score_model) << "\","
             << "\"assessment_profile\":\"" << weaknet_utils::escapeJsonString(row.assessment_profile.empty() ? "UNSPECIFIED" : row.assessment_profile) << "\","
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
        // zero[i] 按 SELECT 列序填充：i 对应 8+i 列，而列 8..12 依次是
        // rtt / rssi / jitter / tcp_loss / traffic_sample_ts。
        // 这里曾把 zero[1] 输出到 "rtt"、zero[0] 输出到 "rssi"，与 status[]
        // 的键序（0=rssi, 1=rtt）混用了两套下标，导致整个 timestamp_zero_counts
        // 的键值对错位。必须按 SELECT 列序命名，不能沿用 status[] 的键序。
        json << "\"rtt\":" << s.zero[0] << ",\"rssi\":" << s.zero[1]
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

    // 同时清理过期的蓝牙历史记录
    std::ostringstream bt_sql;
    bt_sql << "DELETE FROM bt_history WHERE ts < datetime('now', '-"
           << retention_days << " days')";
    char* bt_err = nullptr;
    sqlite3_exec(db_, bt_sql.str().c_str(), nullptr, nullptr, &bt_err);
    if (bt_err) {
        sqlite3_free(bt_err);
    }

    // 同时清理过期的无线设备事件。
    // 注意：device_events.ts 是 Unix 毫秒整数，与上面两张表的 ISO 文本不同，
    // 因此不能用 datetime('now', ...) 直接比较，改用 strftime('%s') 转秒再乘 1000。
    std::ostringstream ev_sql;
    ev_sql << "DELETE FROM device_events WHERE ts < "
           << "(CAST(strftime('%s','now') AS INTEGER) - "
           << retention_days << " * 86400) * 1000";
    char* ev_err = nullptr;
    sqlite3_exec(db_, ev_sql.str().c_str(), nullptr, nullptr, &ev_err);
    if (ev_err) {
        sqlite3_free(ev_err);
    }

    // 同时清理过期的区域级异常事件。与 device_events 同一毫秒时间轴，
    // 因此复用同一条换算表达式（strftime('%s') 转秒再乘 1000）。
    //
    // 先删回链表再删 incident 本体：反向顺序会留下指向已消失 incident 的回链行，
    // 让"某事件曾属于哪起事故"的追溯查询返回空壳。清理本身是有意为之的
    // 数据老化，但不该制造悬垂关系（这两张表刻意没有 FOREIGN KEY，
    // 见 ensureSchema 的说明，所以顺序必须由代码保证）。
    std::ostringstream incident_link_sql;
    incident_link_sql
        << "DELETE FROM site_incident_events WHERE incident_id IN ("
        << "SELECT incident_id FROM site_incidents WHERE last_event_ms < "
        << "(CAST(strftime('%s','now') AS INTEGER) - " << retention_days << " * 86400) * 1000)";
    char* link_err = nullptr;
    sqlite3_exec(db_, incident_link_sql.str().c_str(), nullptr, nullptr, &link_err);
    if (link_err) {
        sqlite3_free(link_err);
    }

    std::ostringstream incident_sql;
    incident_sql << "DELETE FROM site_incidents WHERE last_event_ms < "
                 << "(CAST(strftime('%s','now') AS INTEGER) - "
                 << retention_days << " * 86400) * 1000";
    char* incident_err = nullptr;
    sqlite3_exec(db_, incident_sql.str().c_str(), nullptr, nullptr, &incident_err);
    if (incident_err) {
        sqlite3_free(incident_err);
    }

    return deleted;
}

bool DatabaseManager::insertBtSnapshot(const std::string& adapter_mac,
                                      const std::string& device_mac,
                                      const std::string& device_name,
                                      bool connected,
                                      int16_t rssi_dbm,
                                      double distance_m,
                                      bool audio_active,
                                      double quality_score,
                                      bool suspected_stall,
                                      uint64_t bytes_per_sec,
                                      uint64_t max_gap_ms) {
    if (!db_) return false;

    std::lock_guard<std::mutex> lock(mutex_);

    const char* sql = R"(
        INSERT INTO bt_history (
            ts, adapter_mac, device_mac, device_name,
            connected, rssi_dbm, distance_m, audio_active,
            quality_score, suspected_stall, bytes_per_sec, max_gap_ms
        ) VALUES (
            ?, ?, ?, ?,
            ?, ?, ?, ?,
            ?, ?, ?, ?
        );
    )";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        LOG_ERROR(LogModule::SYSTEM, "DatabaseManager::insertBtSnapshot prepare failed: " << sqlite3_errmsg(db_));
        return false;
    }

    std::string timestamp = currentTimestamp();
    sqlite3_bind_text(stmt, 1, timestamp.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, adapter_mac.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, device_mac.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, device_name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 5, connected ? 1 : 0);
    sqlite3_bind_double(stmt, 6, static_cast<double>(rssi_dbm));
    sqlite3_bind_double(stmt, 7, distance_m);
    sqlite3_bind_int(stmt, 8, audio_active ? 1 : 0);
    sqlite3_bind_double(stmt, 9, quality_score);
    sqlite3_bind_int(stmt, 10, suspected_stall ? 1 : 0);
    sqlite3_bind_int64(stmt, 11, static_cast<int64_t>(bytes_per_sec));
    sqlite3_bind_int64(stmt, 12, static_cast<int64_t>(max_gap_ms));

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        LOG_ERROR(LogModule::SYSTEM, "DatabaseManager::insertBtSnapshot step failed: " << sqlite3_errmsg(db_));
        return false;
    }
    return true;
}

std::string DatabaseManager::queryBtHistory(const std::string& device_mac,
                                            const std::string& start,
                                            const std::string& end,
                                            int limit) {
    if (!db_) return "[]";

    std::lock_guard<std::mutex> lock(mutex_);

    std::ostringstream sql;
    sql << "SELECT ts, adapter_mac, device_mac, device_name, connected, "
        << "rssi_dbm, distance_m, audio_active, quality_score, suspected_stall, "
        << "bytes_per_sec, max_gap_ms FROM bt_history WHERE 1=1";

    if (!device_mac.empty()) {
        sql << " AND device_mac = ?";
    }
    if (!start.empty()) {
        sql << " AND ts >= ?";
    }
    if (!end.empty()) {
        sql << " AND ts <= ?";
    }
    sql << " ORDER BY ts DESC LIMIT ?";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.str().c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        LOG_ERROR(LogModule::SYSTEM, "DatabaseManager::queryBtHistory prepare failed: " << sqlite3_errmsg(db_));
        return "[]";
    }

    int bindIdx = 1;
    if (!device_mac.empty()) {
        sqlite3_bind_text(stmt, bindIdx++, device_mac.c_str(), -1, SQLITE_STATIC);
    }
    if (!start.empty()) {
        sqlite3_bind_text(stmt, bindIdx++, start.c_str(), -1, SQLITE_STATIC);
    }
    if (!end.empty()) {
        sqlite3_bind_text(stmt, bindIdx++, end.c_str(), -1, SQLITE_STATIC);
    }
    sqlite3_bind_int(stmt, bindIdx++, limit > 0 ? limit : 100);

    std::ostringstream json;
    json << "[";
    bool first = true;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (!first) json << ",";
        first = false;

        const char* ts = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const char* adapterMac = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const char* devMac = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        const char* devName = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        int connected = sqlite3_column_int(stmt, 4);
        double rssiDbm = sqlite3_column_double(stmt, 5);
        double distanceM = sqlite3_column_double(stmt, 6);
        int audioActive = sqlite3_column_int(stmt, 7);
        double qualityScore = sqlite3_column_double(stmt, 8);
        int suspectedStall = sqlite3_column_int(stmt, 9);
        int64_t bytesPerSec = sqlite3_column_int64(stmt, 10);
        int64_t maxGapMs = sqlite3_column_int64(stmt, 11);

        json << "{"
             << "\"ts\":\"" << (ts ? weaknet_utils::escapeJsonString(ts) : "") << "\","
             << "\"adapter_mac\":\"" << (adapterMac ? weaknet_utils::escapeJsonString(adapterMac) : "") << "\","
             << "\"device_mac\":\"" << (devMac ? weaknet_utils::escapeJsonString(devMac) : "") << "\","
             << "\"device_name\":\"" << (devName ? weaknet_utils::escapeJsonString(devName) : "") << "\","
             << "\"connected\":" << (connected ? "true" : "false") << ","
             << "\"rssi_dbm\":" << rssiDbm << ","
             << "\"distance_m\":" << distanceM << ","
             << "\"audio_active\":" << (audioActive ? "true" : "false") << ","
             << "\"quality_score\":" << qualityScore << ","
             << "\"suspected_stall\":" << (suspectedStall ? "true" : "false") << ","
             << "\"bytes_per_sec\":" << bytesPerSec << ","
             << "\"max_gap_ms\":" << maxGapMs
             << "}";
    }

    sqlite3_finalize(stmt);
    json << "]";
    return json.str();
}

bool DatabaseManager::insertDeviceEvent(const std::string& event_id,
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
                                        const std::string& details_json) {
    if (!db_) return false;

    std::lock_guard<std::mutex> lock(mutex_);

    const char* sql = R"(
        INSERT OR IGNORE INTO device_events (
            event_id, ts, site_id, gateway_id, protocol,
            device_address, address_type, hci_index, event_type,
            rssi_at_event_dbm, raw_reason_code, reason,
            source, source_detail, suspected_cause, details_json
        ) VALUES (
            ?, ?, ?, ?, ?,
            ?, ?, ?, ?,
            ?, ?, ?,
            ?, ?, ?, ?
        );
    )";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        LOG_ERROR(LogModule::SYSTEM, "DatabaseManager::insertDeviceEvent prepare failed: " << sqlite3_errmsg(db_));
        return false;
    }

    sqlite3_bind_text(stmt, 1, event_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, ts_ms);
    sqlite3_bind_text(stmt, 3, site_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, gateway_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, protocol.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, device_address.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, address_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 8, static_cast<int>(hci_index));
    sqlite3_bind_text(stmt, 9, event_type.c_str(), -1, SQLITE_TRANSIENT);

    // NULL 语义：未采集的 RSSI 写 NULL，不写 0 —— 0 会被后续统计当成真实读数
    if (rssi_dbm.has_value()) {
        sqlite3_bind_int(stmt, 10, *rssi_dbm);
    } else {
        sqlite3_bind_null(stmt, 10);
    }

    sqlite3_bind_int(stmt, 11, static_cast<int>(raw_reason_code));
    sqlite3_bind_text(stmt, 12, reason.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 13, source.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 14, source_detail.c_str(), -1, SQLITE_TRANSIENT);

    // suspected_cause 同为 NULL 语义（Phase 1 恒为空）
    if (suspected_cause.has_value()) {
        sqlite3_bind_text(stmt, 15, suspected_cause->c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, 15);
    }

    sqlite3_bind_text(stmt, 16, details_json.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        LOG_ERROR(LogModule::SYSTEM, "DatabaseManager::insertDeviceEvent step failed: " << sqlite3_errmsg(db_));
        return false;
    }
    return true;
}

std::string DatabaseManager::queryDeviceEvents(const std::string& device_address,
                                               const std::string& event_type,
                                               int64_t start_ms,
                                               int64_t end_ms,
                                               int limit) {
    if (!db_) return "[]";

    std::lock_guard<std::mutex> lock(mutex_);

    std::ostringstream sql;
    sql << "SELECT event_id, ts, site_id, gateway_id, protocol, device_address, "
        << "address_type, hci_index, event_type, rssi_at_event_dbm, raw_reason_code, "
        << "reason, source, source_detail, suspected_cause, details_json "
        << "FROM device_events WHERE 1=1";

    if (!device_address.empty()) {
        sql << " AND device_address = ?";
    }
    if (!event_type.empty()) {
        sql << " AND event_type = ?";
    }
    if (start_ms > 0) {
        sql << " AND ts >= ?";
    }
    if (end_ms > 0) {
        sql << " AND ts <= ?";
    }
    sql << " ORDER BY ts DESC LIMIT ?";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.str().c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        LOG_ERROR(LogModule::SYSTEM, "DatabaseManager::queryDeviceEvents prepare failed: " << sqlite3_errmsg(db_));
        return "[]";
    }

    int bindIdx = 1;
    if (!device_address.empty()) {
        sqlite3_bind_text(stmt, bindIdx++, device_address.c_str(), -1, SQLITE_STATIC);
    }
    if (!event_type.empty()) {
        sqlite3_bind_text(stmt, bindIdx++, event_type.c_str(), -1, SQLITE_STATIC);
    }
    if (start_ms > 0) {
        sqlite3_bind_int64(stmt, bindIdx++, start_ms);
    }
    if (end_ms > 0) {
        sqlite3_bind_int64(stmt, bindIdx++, end_ms);
    }
    sqlite3_bind_int(stmt, bindIdx++, limit > 0 ? limit : 100);

    std::ostringstream json;
    json << "[";
    bool first = true;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (!first) json << ",";
        first = false;

        auto textOrEmpty = [&](int col) -> std::string {
            const char* v = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
            return v ? v : "";
        };

        json << "{"
             << "\"event_id\":\"" << weaknet_utils::escapeJsonString(textOrEmpty(0)) << "\","
             << "\"ts\":" << sqlite3_column_int64(stmt, 1) << ","
             << "\"site_id\":\"" << weaknet_utils::escapeJsonString(textOrEmpty(2)) << "\","
             << "\"gateway_id\":\"" << weaknet_utils::escapeJsonString(textOrEmpty(3)) << "\","
             << "\"protocol\":\"" << weaknet_utils::escapeJsonString(textOrEmpty(4)) << "\","
             << "\"device_address\":\"" << weaknet_utils::escapeJsonString(textOrEmpty(5)) << "\","
             << "\"address_type\":\"" << weaknet_utils::escapeJsonString(textOrEmpty(6)) << "\","
             << "\"hci_index\":" << sqlite3_column_int(stmt, 7) << ","
             << "\"event_type\":\"" << weaknet_utils::escapeJsonString(textOrEmpty(8)) << "\","
             << "\"rssi_at_event_dbm\":";
        // 回读时同样保持 NULL 语义：未采集输出 null 而不是 0
        if (sqlite3_column_type(stmt, 9) == SQLITE_NULL) {
            json << "null";
        } else {
            json << sqlite3_column_int(stmt, 9);
        }
        json << ",\"raw_reason_code\":" << sqlite3_column_int(stmt, 10) << ","
             << "\"reason\":\"" << weaknet_utils::escapeJsonString(textOrEmpty(11)) << "\","
             << "\"source\":\"" << weaknet_utils::escapeJsonString(textOrEmpty(12)) << "\","
             << "\"source_detail\":\"" << weaknet_utils::escapeJsonString(textOrEmpty(13)) << "\","
             << "\"suspected_cause\":";
        if (sqlite3_column_type(stmt, 14) == SQLITE_NULL) {
            json << "null";
        } else {
            json << "\"" << weaknet_utils::escapeJsonString(textOrEmpty(14)) << "\"";
        }
        json << ",\"details\":" << (textOrEmpty(15).empty() ? "{}" : textOrEmpty(15))
             << "}";
    }

    sqlite3_finalize(stmt);
    json << "]";
    return json.str();
}

bool DatabaseManager::upsertDeviceBaseline(const std::string& site_id,
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
                                          int64_t updated_at_ms) {
    if (!db_) return false;

    std::lock_guard<std::mutex> lock(mutex_);

    const char* sql = R"(
        INSERT INTO device_baselines (
            site_id, gateway_id, hci_index, protocol, address_type, device_address,
            baseline_rssi_dbm, min_seen_rssi_dbm, max_seen_rssi_dbm, baseline_sample_count,
            first_seen_ms, last_seen_ms, state, updated_at
        ) VALUES (
            ?, ?, ?, ?, ?, ?,
            ?, ?, ?, ?,
            ?, ?, ?, ?
        )
        ON CONFLICT(site_id, gateway_id, hci_index, protocol, address_type, device_address)
        DO UPDATE SET
            baseline_rssi_dbm = excluded.baseline_rssi_dbm,
            min_seen_rssi_dbm = excluded.min_seen_rssi_dbm,
            max_seen_rssi_dbm = excluded.max_seen_rssi_dbm,
            baseline_sample_count = excluded.baseline_sample_count,
            last_seen_ms = excluded.last_seen_ms,
            state = excluded.state,
            updated_at = excluded.updated_at;
    )";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        LOG_ERROR(LogModule::SYSTEM, "DatabaseManager::upsertDeviceBaseline prepare failed: " << sqlite3_errmsg(db_));
        return false;
    }

    sqlite3_bind_text(stmt, 1, site_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, gateway_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, static_cast<int>(hci_index));
    sqlite3_bind_text(stmt, 4, protocol.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, address_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, device_address.c_str(), -1, SQLITE_TRANSIENT);

    if (baseline_rssi_dbm.has_value()) {
        sqlite3_bind_int(stmt, 7, *baseline_rssi_dbm);
    } else {
        sqlite3_bind_null(stmt, 7);
    }

    if (min_seen_rssi_dbm.has_value()) {
        sqlite3_bind_int(stmt, 8, *min_seen_rssi_dbm);
    } else {
        sqlite3_bind_null(stmt, 8);
    }

    if (max_seen_rssi_dbm.has_value()) {
        sqlite3_bind_int(stmt, 9, *max_seen_rssi_dbm);
    } else {
        sqlite3_bind_null(stmt, 9);
    }

    sqlite3_bind_int(stmt, 10, static_cast<int>(baseline_sample_count));
    sqlite3_bind_int64(stmt, 11, first_seen_ms);
    sqlite3_bind_int64(stmt, 12, last_seen_ms);
    sqlite3_bind_text(stmt, 13, state.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 14, updated_at_ms);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        LOG_ERROR(LogModule::SYSTEM, "DatabaseManager::upsertDeviceBaseline step failed: " << sqlite3_errmsg(db_));
        return false;
    }
    return true;
}

std::string DatabaseManager::queryDeviceBaselines(const std::string& device_address,
                                                 int limit) {
    if (!db_) return "[]";

    std::lock_guard<std::mutex> lock(mutex_);

    std::ostringstream sql;
    sql << "SELECT site_id, gateway_id, hci_index, protocol, address_type, device_address, "
        << "baseline_rssi_dbm, min_seen_rssi_dbm, max_seen_rssi_dbm, baseline_sample_count, "
        << "first_seen_ms, last_seen_ms, state, updated_at "
        << "FROM device_baselines WHERE 1=1";

    if (!device_address.empty()) {
        sql << " AND device_address = ?";
    }
    sql << " ORDER BY updated_at DESC LIMIT ?";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.str().c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        LOG_ERROR(LogModule::SYSTEM, "DatabaseManager::queryDeviceBaselines prepare failed: " << sqlite3_errmsg(db_));
        return "[]";
    }

    int bindIdx = 1;
    if (!device_address.empty()) {
        sqlite3_bind_text(stmt, bindIdx++, device_address.c_str(), -1, SQLITE_STATIC);
    }
    sqlite3_bind_int(stmt, bindIdx++, limit > 0 ? limit : 100);

    std::ostringstream json;
    json << "[";
    bool first = true;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (!first) json << ",";
        first = false;

        auto textOrEmpty = [&](int col) -> std::string {
            const char* v = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
            return v ? v : "";
        };

        json << "{"
             << "\"site_id\":\"" << weaknet_utils::escapeJsonString(textOrEmpty(0)) << "\","
             << "\"gateway_id\":\"" << weaknet_utils::escapeJsonString(textOrEmpty(1)) << "\","
             << "\"hci_index\":" << sqlite3_column_int(stmt, 2) << ","
             << "\"protocol\":\"" << weaknet_utils::escapeJsonString(textOrEmpty(3)) << "\","
             << "\"address_type\":\"" << weaknet_utils::escapeJsonString(textOrEmpty(4)) << "\","
             << "\"device_address\":\"" << weaknet_utils::escapeJsonString(textOrEmpty(5)) << "\","
             << "\"baseline_rssi_dbm\":";
        if (sqlite3_column_type(stmt, 6) == SQLITE_NULL) {
            json << "null";
        } else {
            json << sqlite3_column_int(stmt, 6);
        }
        json << ",\"min_seen_rssi_dbm\":";
        if (sqlite3_column_type(stmt, 7) == SQLITE_NULL) {
            json << "null";
        } else {
            json << sqlite3_column_int(stmt, 7);
        }
        json << ",\"max_seen_rssi_dbm\":";
        if (sqlite3_column_type(stmt, 8) == SQLITE_NULL) {
            json << "null";
        } else {
            json << sqlite3_column_int(stmt, 8);
        }
        json << ",\"baseline_sample_count\":" << sqlite3_column_int(stmt, 9) << ","
             << "\"first_seen_ms\":" << sqlite3_column_int64(stmt, 10) << ","
             << "\"last_seen_ms\":" << sqlite3_column_int64(stmt, 11) << ","
             << "\"state\":\"" << weaknet_utils::escapeJsonString(textOrEmpty(12)) << "\","
             << "\"updated_at\":" << sqlite3_column_int64(stmt, 13)
             << "}";
    }

    sqlite3_finalize(stmt);
    json << "]";
    return json.str();
}

bool DatabaseManager::upsertSiteIncident(const std::string& incident_id,
                                         const std::string& site_id,
                                         const std::string& gateway_id,
                                         int64_t started_at_ms,
                                         int64_t last_event_ms,
                                         const std::optional<int64_t>& resolved_at_ms,
                                         int64_t affected_devices,
                                         const std::string& state,
                                         const std::optional<std::string>& suspected_cause) {
    if (!db_) return false;

    std::lock_guard<std::mutex> lock(mutex_);

    // UPSERT 而非 INSERT OR IGNORE：关联器在每次状态推进（新开/吸收/结案）都写回
    // 同一行，重启回放重新推导出的确定性 incident_id 也必须更新而不是被忽略——
    // 否则库里永远停在第一次开事故时的 OPEN 快照。
    const char* sql = R"(
        INSERT INTO site_incidents (
            incident_id, site_id, gateway_id,
            started_at_ms, last_event_ms, resolved_at_ms,
            affected_devices, state, suspected_cause
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(incident_id)
        DO UPDATE SET
            site_id          = excluded.site_id,
            gateway_id       = excluded.gateway_id,
            started_at_ms    = excluded.started_at_ms,
            last_event_ms    = excluded.last_event_ms,
            resolved_at_ms   = excluded.resolved_at_ms,
            affected_devices = excluded.affected_devices,
            state            = excluded.state,
            suspected_cause  = excluded.suspected_cause;
    )";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        LOG_ERROR(LogModule::SYSTEM,
                  "DatabaseManager::upsertSiteIncident prepare failed: " << sqlite3_errmsg(db_));
        return false;
    }

    sqlite3_bind_text(stmt, 1, incident_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, site_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, gateway_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, started_at_ms);
    sqlite3_bind_int64(stmt, 5, last_event_ms);

    // NULL 语义：仍在活跃时 resolved_at_ms 写 NULL，不写 0——0 会造出
    // "1970 年结案"这种假事实，并让"是否已结案"的判断永远为真。
    if (resolved_at_ms.has_value()) {
        sqlite3_bind_int64(stmt, 6, *resolved_at_ms);
    } else {
        sqlite3_bind_null(stmt, 6);
    }

    sqlite3_bind_int64(stmt, 7, affected_devices);
    sqlite3_bind_text(stmt, 8, state.c_str(), -1, SQLITE_TRANSIENT);

    // suspected_cause 同为 NULL 语义（Phase 3a 恒为空：关联层不做根因推断）
    if (suspected_cause.has_value()) {
        sqlite3_bind_text(stmt, 9, suspected_cause->c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, 9);
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        LOG_ERROR(LogModule::SYSTEM,
                  "DatabaseManager::upsertSiteIncident step failed: " << sqlite3_errmsg(db_));
        return false;
    }
    return true;
}

bool DatabaseManager::insertSiteIncidentEvent(const std::string& incident_id,
                                              const std::string& event_id) {
    if (!db_) return false;

    std::lock_guard<std::mutex> lock(mutex_);

    // OR IGNORE：incident 的每一次写回都会重放它的全部事件回链，
    // 复合主键让重复插入成为无操作，回放/重试因此天然幂等。
    const char* sql = R"(
        INSERT OR IGNORE INTO site_incident_events (incident_id, event_id)
        VALUES (?, ?);
    )";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        LOG_ERROR(LogModule::SYSTEM,
                  "DatabaseManager::insertSiteIncidentEvent prepare failed: "
                      << sqlite3_errmsg(db_));
        return false;
    }

    sqlite3_bind_text(stmt, 1, incident_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, event_id.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        LOG_ERROR(LogModule::SYSTEM,
                  "DatabaseManager::insertSiteIncidentEvent step failed: "
                      << sqlite3_errmsg(db_));
        return false;
    }
    return true;
}

std::string DatabaseManager::querySiteIncidents(const std::string& state,
                                                int64_t start_ms,
                                                int64_t end_ms,
                                                int limit,
                                                bool include_devices) {
    if (!db_) return "[]";

    std::lock_guard<std::mutex> lock(mutex_);

    std::ostringstream sql;
    sql << "SELECT incident_id, site_id, gateway_id, started_at_ms, last_event_ms, "
        << "resolved_at_ms, affected_devices, state, suspected_cause "
        << "FROM site_incidents WHERE 1=1";

    if (!state.empty()) {
        sql << " AND state = ?";
    }
    // 时间过滤以 last_event_ms 为准：用户问"凌晨 3 点发生了什么"时，
    // 一起跨 3 点的事故应当落进 3 点这个窗口，而不是因为它开始于 2:58 而消失。
    if (start_ms > 0) {
        sql << " AND last_event_ms >= ?";
    }
    if (end_ms > 0) {
        sql << " AND last_event_ms <= ?";
    }
    sql << " ORDER BY last_event_ms DESC LIMIT ?";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.str().c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        LOG_ERROR(LogModule::SYSTEM,
                  "DatabaseManager::querySiteIncidents prepare failed: " << sqlite3_errmsg(db_));
        return "[]";
    }

    int bindIdx = 1;
    if (!state.empty()) {
        sqlite3_bind_text(stmt, bindIdx++, state.c_str(), -1, SQLITE_STATIC);
    }
    if (start_ms > 0) {
        sqlite3_bind_int64(stmt, bindIdx++, start_ms);
    }
    if (end_ms > 0) {
        sqlite3_bind_int64(stmt, bindIdx++, end_ms);
    }
    sqlite3_bind_int(stmt, bindIdx++, limit > 0 ? limit : 100);

    std::ostringstream json;
    json << "[";
    bool first = true;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (!first) json << ",";
        first = false;

        auto textOrEmpty = [&](int col) -> std::string {
            const char* v = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
            return v ? v : "";
        };

        const std::string incident_id = textOrEmpty(0);

        json << "{"
             << "\"incident_id\":\"" << weaknet_utils::escapeJsonString(incident_id) << "\","
             << "\"site_id\":\"" << weaknet_utils::escapeJsonString(textOrEmpty(1)) << "\","
             << "\"gateway_id\":\"" << weaknet_utils::escapeJsonString(textOrEmpty(2)) << "\","
             << "\"started_at_ms\":" << sqlite3_column_int64(stmt, 3) << ","
             << "\"last_event_ms\":" << sqlite3_column_int64(stmt, 4) << ","
             << "\"resolved_at_ms\":";
        // 回读保持 NULL 语义：活跃 incident 输出 null 而不是 0
        if (sqlite3_column_type(stmt, 5) == SQLITE_NULL) {
            json << "null";
        } else {
            json << sqlite3_column_int64(stmt, 5);
        }
        json << ",\"affected_devices\":" << sqlite3_column_int64(stmt, 6) << ","
             << "\"state\":\"" << weaknet_utils::escapeJsonString(textOrEmpty(7)) << "\","
             << "\"suspected_cause\":";
        if (sqlite3_column_type(stmt, 8) == SQLITE_NULL) {
            json << "null";
        } else {
            json << "\"" << weaknet_utils::escapeJsonString(textOrEmpty(8)) << "\"";
        }

        // 受影响设备清单：从回链表派生（不额外存第二份事实）。
        // 用嵌套查询而不是 JOIN + GROUP_CONCAT：后者在设备地址含分隔符时
        // 会被静默切错，而设备地址是外部观测数据，不能假设它干净。
        if (include_devices) {
            json << ",\"affected_device_ids\":[";

            sqlite3_stmt* dev_stmt = nullptr;
            const char* dev_sql =
                "SELECT DISTINCT d.device_address "
                "FROM site_incident_events e JOIN device_events d ON d.event_id = e.event_id "
                "WHERE e.incident_id = ? ORDER BY d.device_address LIMIT ?";
            if (sqlite3_prepare_v2(db_, dev_sql, -1, &dev_stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(dev_stmt, 1, incident_id.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(dev_stmt, 2, 1000);
                bool dev_first = true;
                while (sqlite3_step(dev_stmt) == SQLITE_ROW) {
                    if (!dev_first) json << ",";
                    dev_first = false;
                    const char* addr =
                        reinterpret_cast<const char*>(sqlite3_column_text(dev_stmt, 0));
                    json << "\"" << weaknet_utils::escapeJsonString(addr ? addr : "") << "\"";
                }
                sqlite3_finalize(dev_stmt);
            }
            json << "]";
        }

        json << "}";
    }

    sqlite3_finalize(stmt);
    json << "]";
    return json.str();
}

int64_t DatabaseManager::queryLatestIncidentResolvedAt(const std::string& site_id) {
    if (!db_) return 0;

    std::lock_guard<std::mutex> lock(mutex_);

    // 只认已结案的行（resolved_at_ms IS NOT NULL）：活跃 incident 不构成
    // 回放下限，否则回放会因为"上一起还没结案"而完全跳过证据。
    int64_t latest = 0;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT MAX(resolved_at_ms) FROM site_incidents "
        "WHERE resolved_at_ms IS NOT NULL AND (? = '' OR site_id = ?)";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        LOG_ERROR(LogModule::SYSTEM,
                  "DatabaseManager::queryLatestIncidentResolvedAt prepare failed: "
                      << sqlite3_errmsg(db_));
        return 0;
    }
    sqlite3_bind_text(stmt, 1, site_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, site_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
        latest = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return latest;
}

std::vector<WirelessDeviceEvent> DatabaseManager::loadDeviceEventsForReplay(int64_t start_ms,
                                                                           int64_t end_ms) {
    std::vector<WirelessDeviceEvent> out;
    if (!db_) return out;

    std::lock_guard<std::mutex> lock(mutex_);

    // 与 queryDeviceEvents 的区别是 **ORDER BY ts ASC**：回放必须按时间正序
    // 重演关联决策，倒序会让"谁先到"完全颠倒。
    const char* sql = R"(
        SELECT event_id, ts, site_id, gateway_id, protocol, device_address,
               address_type, hci_index, event_type, rssi_at_event_dbm, raw_reason_code,
               reason, source, source_detail, suspected_cause, details_json
        FROM device_events
        WHERE ts >= ? AND ts <= ?
        ORDER BY ts ASC
        LIMIT 100000;
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        LOG_ERROR(LogModule::SYSTEM,
                  "DatabaseManager::loadDeviceEventsForReplay prepare failed: "
                      << sqlite3_errmsg(db_));
        return out;
    }

    sqlite3_bind_int64(stmt, 1, start_ms);
    sqlite3_bind_int64(stmt, 2, end_ms);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        auto textOrEmpty = [&](int col) -> std::string {
            const char* v = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
            return v ? v : "";
        };

        WirelessDeviceEvent ev;
        ev.event_id = textOrEmpty(0);
        ev.timestamp_ms = static_cast<uint64_t>(sqlite3_column_int64(stmt, 1));
        ev.site_id = textOrEmpty(2);
        ev.gateway_id = textOrEmpty(3);
        ev.protocol = wirelessProtocolFromString(textOrEmpty(4), WirelessProtocol::Bluetooth);
        ev.device_address = textOrEmpty(5);
        ev.address_type = btAddressTypeFromString(textOrEmpty(6), BtAddressType::Unknown);
        ev.hci_index = static_cast<uint32_t>(sqlite3_column_int(stmt, 7));
        ev.event_type =
            deviceEventTypeFromString(textOrEmpty(8), DeviceEventType::LinkDisconnected);
        ev.rssi_at_event_dbm = (sqlite3_column_type(stmt, 9) == SQLITE_NULL)
                                   ? std::nullopt
                                   : std::optional<int>(sqlite3_column_int(stmt, 9));
        ev.raw_reason_code = static_cast<uint8_t>(sqlite3_column_int(stmt, 10));
        ev.reason = disconnectReasonFromString(textOrEmpty(11), DisconnectReason::Unknown);
        ev.source = evidenceSourceFromString(textOrEmpty(12), EvidenceSource::Unknown);
        ev.source_detail = textOrEmpty(13);
        ev.suspected_cause = (sqlite3_column_type(stmt, 14) == SQLITE_NULL)
                                 ? std::nullopt
                                 : std::optional<std::string>(textOrEmpty(14));
        ev.details_json = textOrEmpty(15);
        // monotonic_ns 不落库（它是进程内单调时钟，跨重启无意义）。
        // 回放只重建时空关联所需的墙钟时间线，因此这里保持 0。

        out.push_back(std::move(ev));
    }
    sqlite3_finalize(stmt);
    return out;
}

std::vector<SiteIncident> DatabaseManager::loadSiteIncidentsSince(int64_t start_ms, int limit) {
    std::vector<SiteIncident> out;
    if (!db_) return out;

    std::lock_guard<std::mutex> lock(mutex_);

    // 正序 + 按 last_event_ms 过滤：上行按"事故最近演化时间"推进，
    // 这样一起仍在吸收证据的事故会被反复重发（云端 UPSERT 幂等）。
    const char* sql = R"(
        SELECT incident_id, site_id, gateway_id, started_at_ms, last_event_ms,
               resolved_at_ms, affected_devices, state, suspected_cause
        FROM site_incidents
        WHERE last_event_ms >= ?
        ORDER BY last_event_ms ASC
        LIMIT ?;
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        LOG_ERROR(LogModule::SYSTEM, "DatabaseManager::loadSiteIncidentsSince prepare failed: "
                                         << sqlite3_errmsg(db_));
        return out;
    }
    sqlite3_bind_int64(stmt, 1, start_ms);
    sqlite3_bind_int(stmt, 2, limit > 0 ? limit : 200);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        auto textOrEmpty = [&](int col) -> std::string {
            const char* v = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
            return v ? v : "";
        };

        SiteIncident inc;
        inc.incident_id = textOrEmpty(0);
        inc.site_id = textOrEmpty(1);
        inc.gateway_id = textOrEmpty(2);
        inc.started_at_ms = static_cast<uint64_t>(sqlite3_column_int64(stmt, 3));
        inc.last_event_ms = static_cast<uint64_t>(sqlite3_column_int64(stmt, 4));
        inc.resolved_at_ms = (sqlite3_column_type(stmt, 5) == SQLITE_NULL)
                                 ? std::nullopt
                                 : std::optional<uint64_t>(
                                       static_cast<uint64_t>(sqlite3_column_int64(stmt, 5)));
        inc.affected_devices = static_cast<size_t>(sqlite3_column_int64(stmt, 6));
        inc.state = incidentStateFromString(textOrEmpty(7), IncidentState::Open);
        inc.suspected_cause = (sqlite3_column_type(stmt, 8) == SQLITE_NULL)
                                  ? std::nullopt
                                  : std::optional<std::string>(textOrEmpty(8));
        out.push_back(std::move(inc));
    }
    sqlite3_finalize(stmt);
    return out;
}

std::vector<std::string> DatabaseManager::loadIncidentEvidenceEventIds(
    const std::string& incident_id) {
    std::vector<std::string> out;
    if (!db_) return out;

    std::lock_guard<std::mutex> lock(mutex_);

    const char* sql = "SELECT event_id FROM site_incident_events WHERE incident_id = ? "
                      "ORDER BY event_id ASC;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        LOG_ERROR(LogModule::SYSTEM,
                  "DatabaseManager::loadIncidentEvidenceEventIds prepare failed: "
                      << sqlite3_errmsg(db_));
        return out;
    }
    sqlite3_bind_text(stmt, 1, incident_id.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* v = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (v) out.emplace_back(v);
    }
    sqlite3_finalize(stmt);
    return out;
}

std::vector<DeviceLinkProfile> DatabaseManager::loadDeviceBaselinesSince(
    int64_t since_updated_at_ms, int limit) {
    std::vector<DeviceLinkProfile> out;
    if (!db_) return out;

    std::lock_guard<std::mutex> lock(mutex_);

    // 稳定顺序（updated_at 有并列时用复合主键收尾），保证游标推进不漏不重。
    const char* sql = R"(
        SELECT site_id, gateway_id, hci_index, protocol, address_type, device_address,
               baseline_rssi_dbm, min_seen_rssi_dbm, max_seen_rssi_dbm, baseline_sample_count,
               first_seen_ms, last_seen_ms, state, updated_at
        FROM device_baselines
        WHERE updated_at > ?
        ORDER BY updated_at ASC, site_id ASC, gateway_id ASC, hci_index ASC,
                 protocol ASC, address_type ASC, device_address ASC
        LIMIT ?;
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        LOG_ERROR(LogModule::SYSTEM, "DatabaseManager::loadDeviceBaselinesSince prepare failed: "
                                         << sqlite3_errmsg(db_));
        return out;
    }
    sqlite3_bind_int64(stmt, 1, since_updated_at_ms);
    sqlite3_bind_int(stmt, 2, limit > 0 ? limit : 2000);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        auto textOrEmpty = [&](int col) -> std::string {
            const char* v = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
            return v ? v : "";
        };
        auto optInt = [&](int col) -> std::optional<int16_t> {
            if (sqlite3_column_type(stmt, col) == SQLITE_NULL) return std::nullopt;
            return static_cast<int16_t>(sqlite3_column_int(stmt, col));
        };

        DeviceLinkProfile p;
        p.key.site_id = textOrEmpty(0);
        p.key.gateway_id = textOrEmpty(1);
        p.key.hci_index = static_cast<uint32_t>(sqlite3_column_int(stmt, 2));
        p.key.protocol = wirelessProtocolFromString(textOrEmpty(3), WirelessProtocol::Bluetooth);
        p.key.address_type = btAddressTypeFromString(textOrEmpty(4), BtAddressType::Unknown);
        p.key.device_address = textOrEmpty(5);
        p.baseline_rssi_dbm = optInt(6);
        p.min_seen_rssi_dbm = optInt(7);
        p.max_seen_rssi_dbm = optInt(8);
        p.baseline_sample_count = static_cast<size_t>(sqlite3_column_int64(stmt, 9));
        p.first_seen_ms = static_cast<uint64_t>(sqlite3_column_int64(stmt, 10));
        p.last_seen_ms = static_cast<uint64_t>(sqlite3_column_int64(stmt, 11));
        p.state = linkQualityStateFromString(textOrEmpty(12), LinkQualityState::Learning);
        p.updated_at_ms = static_cast<uint64_t>(sqlite3_column_int64(stmt, 13));
        out.push_back(std::move(p));
    }
    sqlite3_finalize(stmt);
    return out;
}

int64_t DatabaseManager::queryMaxDeviceEventTs() {
    if (!db_) return 0;

    std::lock_guard<std::mutex> lock(mutex_);

    int64_t max_ts = 0;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT MAX(ts) FROM device_events", -1, &stmt, nullptr) !=
        SQLITE_OK) {
        LOG_ERROR(LogModule::SYSTEM, "DatabaseManager::queryMaxDeviceEventTs prepare failed: "
                                         << sqlite3_errmsg(db_));
        return 0;
    }
    if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
        max_ts = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return max_ts;
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
