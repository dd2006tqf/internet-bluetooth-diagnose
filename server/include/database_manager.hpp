// database_manager.hpp
// SQLite 历史数据持久化管理器

#pragma once

#include <string>
#include <mutex>
#include <cstdint>

struct sqlite3;

namespace weaknet_dbus {

class NetInfo;

class DatabaseManager {
public:
    explicit DatabaseManager(const std::string& db_path);
    ~DatabaseManager();

    // 禁止拷贝
    DatabaseManager(const DatabaseManager&) = delete;
    DatabaseManager& operator=(const DatabaseManager&) = delete;

    bool isOpen() const;

    // 写入一条快照记录
    bool insertSnapshot(const std::string& iface, const NetInfo& info);

    // 查询历史数据，返回 JSON 数组字符串
    // interface: "" 表示所有网卡
    // start/end: ISO 8601 时间范围，"" 表示不限
    // limit: 最大返回行数
    std::string queryHistory(const std::string& interface,
                             const std::string& start,
                             const std::string& end,
                             int limit = 100);

    // 清理过期数据，返回删除行数
    int cleanup(int retention_days = 7);

    // 统计信息
    int64_t getRecordCount();
    std::string getDbInfo();

private:
    bool exec(const std::string& sql);
    bool ensureSchema();

    sqlite3* db_ = nullptr;
    std::mutex write_mutex_;
};

}  // namespace weaknet_dbus
