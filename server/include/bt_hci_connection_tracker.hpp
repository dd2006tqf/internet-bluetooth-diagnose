#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace weaknet {

/// 三元组连接身份主键：控制器 + 12位句柄 + 连接代次（防御句柄复用串设备）
struct HciConnectionKey {
    std::string controller_id;
    uint16_t handle{0};
    uint32_t generation{0};

    bool operator==(const HciConnectionKey& other) const {
        return controller_id == other.controller_id &&
               handle == other.handle &&
               generation == other.generation;
    }
};

struct HciConnectionKeyHash {
    std::size_t operator()(const HciConnectionKey& k) const {
        std::size_t h1 = std::hash<std::string>{}(k.controller_id);
        std::size_t h2 = std::hash<uint16_t>{}(k.handle);
        std::size_t h3 = std::hash<uint32_t>{}(k.generation);
        return h1 ^ (h2 << 1) ^ (h3 << 2);
    }
};

struct HciConnectionRecord {
    HciConnectionKey key;
    std::string device_mac;     // 大写规整 MAC，若未知为空
    std::string address_type;   // "PUBLIC" | "RANDOM" | "UNKNOWN"
    std::string attribution_scope; // "device" | "controller" | "unknown"
    std::chrono::steady_clock::time_point connected_at;
    std::optional<std::chrono::steady_clock::time_point> disconnected_at;
    bool is_active{true};
};

class BtHciConnectionTracker {
public:
    BtHciConnectionTracker() = default;
    ~BtHciConnectionTracker() = default;

    BtHciConnectionTracker(const BtHciConnectionTracker&) = delete;
    BtHciConnectionTracker& operator=(const BtHciConnectionTracker&) = delete;

    /**
     * @brief 记录连接建立事件
     * @param controller 控制器名（如 "hci0"）
     * @param handle 12 位连接句柄（0..4095）
     * @param mac 关联设备 MAC（可选，未知时传空）
     * @param addr_type 地址类型
     * @return 当前分配的稳定 HciConnectionKey
     */
    HciConnectionKey onConnectionEstablished(const std::string& controller,
                                             uint16_t handle,
                                             const std::string& mac = "",
                                             const std::string& addr_type = "UNKNOWN");

    /**
     * @brief 记录连接断开事件
     * @return true 若找到并成功标记断开；false 句柄未曾记录
     */
    bool onConnectionDisconnected(const std::string& controller, uint16_t handle);

    /**
     * @brief 按当前活跃句柄查找身份
     */
    std::optional<HciConnectionRecord> getActiveConnection(const std::string& controller,
                                                           uint16_t handle) const;

    /**
     * @brief 获取指定控制器的所有活跃连接快照
     */
    std::vector<HciConnectionRecord> getActiveConnections(const std::string& controller) const;

    /**
     * @brief 控制器复位时清理活动连接（代次历史继续递增以防复用）
     */
    void resetController(const std::string& controller);

    size_t activeCount() const;

private:
    mutable std::mutex mutex_;

    // 每个控制器下每个 handle 当前分配到的最大代次：key = "controller:handle" -> generation
    std::unordered_map<std::string, uint32_t> generation_counter_;

    // 当前活跃句柄映射：key = "controller:handle" -> HciConnectionRecord
    std::unordered_map<std::string, HciConnectionRecord> active_map_;
};

} // namespace weaknet
