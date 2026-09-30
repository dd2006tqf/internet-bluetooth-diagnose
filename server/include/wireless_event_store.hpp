/**
 * @file wireless_event_store.hpp
 * @brief 规范化无线设备事件的记录与查询（内存环形缓冲 + SQLite 落库）
 *
 * 本类是 BtEventNormalizer 的下游、DatabaseManager 的上游：
 *
 *   BtEventNormalizer（归一化）
 *        ↓ WirelessDeviceEvent（Canonical Device Event）
 *   WirelessEventStore::recordEvent()   ← 本类
 *        ├── 内存环形缓冲（最近 N 条，供低延迟查询）
 *        └── DatabaseManager::insertDeviceEvent()（持久化）
 *        ↓
 *   device_events 表
 *
 * 职责边界：
 *   - **不产生事件**：事件由 normalizer 产出，store 只负责记录
 *   - **不做诊断**：suspected_cause 一律透传 normalizer 的值（Phase 1 为空）
 *   - **不做归一化**：不在本层做任何去重/合并，那是 normalizer 的职责
 *
 * 线程安全：所有公开方法由内部 mutex 保护。可在消费线程写入、
 * 查询线程读取，无需外部加锁。
 */

#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "wireless_event.hpp"

namespace weaknet_dbus {

class DatabaseManager;  ///< 前置声明

/**
 * @brief 事件存储配置
 */
struct WirelessEventStoreConfig {
    /// 内存环形缓冲上限（超出后淘汰最旧）。仅影响内存查询，
    /// 落库数据不受此限制影响。
    size_t ring_capacity = 1000;
    /// site_id：现场身份。Phase 1 运行模型为"1 Gateway = 1 Site"，
    /// 因此默认取 gateway_id。多网关组网时由上层显式覆盖。
    std::string site_id;
    /// 网关身份，用于补齐事件缺少的来源身份字段
    std::string gateway_id;
};

/**
 * @brief 规范化无线设备事件存储
 */
class WirelessEventStore {
public:
    /**
     * @param db 数据库管理器；可为 nullptr（此时仅内存缓冲，用于单元测试
     *           或降级场景）。store 不拥有 db 的生命周期。
     */
    explicit WirelessEventStore(DatabaseManager* db,
                                WirelessEventStoreConfig cfg = {});

    /**
     * @brief 记录一条规范化事件
     *
     * 补齐 site_id / gateway_id（若事件自身未携带），写入内存环形缓冲，
     * 并尝试落库。落库失败不抛异常——事件仍在内存缓冲中可查，
     * 失败原因记日志。
     *
     * @return true 表示内存记录成功（不代表落库成功）
     */
    bool recordEvent(const WirelessDeviceEvent& event);

    /**
     * @brief 从内存环形缓冲查询最近的事件（低延迟，不查库）
     *
     * @param device_address 设备地址过滤，"" 表示所有设备
     * @param limit          最大返回条数
     * @return 按时间倒序的事件副本
     */
    std::vector<WirelessDeviceEvent> recentEvents(const std::string& device_address,
                                                   size_t limit = 50) const;

    /// 已记录事件总数（自进程启动起，含未落库的）
    uint64_t totalRecorded() const;

    /// 落库失败次数（诊断用：持续增长说明 DB 异常）
    uint64_t persistFailures() const;

    /**
     * @brief 从持久化层查询事件（走 DatabaseManager，返回 JSON 数组）
     *
     * 事件查询的系统出口：D-Bus QueryDeviceEvents 与 history_query_tool
     * --events 都经由此处，调用方不应绕过它直接拼 SQL。
     *
     * @param device_address 设备地址过滤，"" 表示所有设备
     * @param event_type     事件类型过滤（如 "LINK_DISCONNECTED"），"" 表示所有类型
     * @param start_ms       起始时间（Unix 毫秒），0 表示不限
     * @param end_ms         结束时间（Unix 毫秒），0 表示不限
     * @param limit          最大条数
     * @return JSON 数组字符串；db 为空时返回 "[]"
     */
    std::string queryPersisted(const std::string& device_address,
                               const std::string& event_type,
                               int64_t start_ms,
                               int64_t end_ms,
                               int limit = 100) const;

    /**
     * @brief 持久化设备基线画像（代理至 DatabaseManager::upsertDeviceBaseline）
     */
    bool saveDeviceBaseline(const DeviceLinkProfile& profile);

    /**
     * @brief 从持久化层查询设备基线画像列表
     */
    std::string queryDeviceBaselines(const std::string& device_address = "",
                                    int limit = 100) const;

    /// 清空内存环形缓冲（不影响已落库数据）
    void clearMemory();

    const WirelessEventStoreConfig& config() const { return cfg_; }

private:
    DatabaseManager* db_;              ///< 不拥有所有权
    WirelessEventStoreConfig cfg_;
    mutable std::mutex mu_;
    std::deque<WirelessDeviceEvent> ring_;  ///< 内存环形缓冲（时间正序）
    uint64_t total_recorded_ = 0;
    uint64_t persist_failures_ = 0;
};

}  // namespace weaknet_dbus
