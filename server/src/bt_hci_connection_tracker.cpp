#include "bt_hci_connection_tracker.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <set>

namespace weaknet {

namespace {

std::string makeSlotKey(const std::string& controller, uint16_t handle) {
    return controller + ":" + std::to_string(handle & 0x0FFF);
}

std::string normalizeMac(const std::string& mac) {
    if (mac.empty()) return "";
    std::string out;
    out.reserve(mac.size());
    for (char c : mac) {
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return out;
}

} // namespace

HciConnectionKey BtHciConnectionTracker::onConnectionEstablished(
    const std::string& controller,
    uint16_t handle,
    const std::string& mac,
    const std::string& addr_type) {

    uint16_t valid_handle = handle & 0x0FFF;
    std::string slot_key = makeSlotKey(controller, valid_handle);
    std::string clean_mac = normalizeMac(mac);

    std::lock_guard<std::mutex> lock(mutex_);

    auto it = active_map_.find(slot_key);
    if (it != active_map_.end() && it->second.is_active) {
        // 重复通知幂等处理：若 MAC 一致，直接复用当前代次
        if (!clean_mac.empty() && it->second.device_mac == clean_mac) {
            return it->second.key;
        }
        // 若 MAC 发生冲突或前连接未正常收到 disconnect，强制递增代次
        it->second.is_active = false;
        it->second.disconnected_at = std::chrono::steady_clock::now();
    }

    uint32_t next_gen = ++generation_counter_[slot_key];

    HciConnectionKey key{controller, valid_handle, next_gen};
    HciConnectionRecord record;
    record.key = key;
    record.device_mac = clean_mac;
    record.address_type = addr_type.empty() ? "UNKNOWN" : addr_type;
    record.attribution_scope = clean_mac.empty() ? "controller" : "device";
    record.connected_at = std::chrono::steady_clock::now();
    record.is_active = true;

    active_map_[slot_key] = record;
    return key;
}

bool BtHciConnectionTracker::onConnectionDisconnected(
    const std::string& controller, uint16_t handle) {

    uint16_t valid_handle = handle & 0x0FFF;
    std::string slot_key = makeSlotKey(controller, valid_handle);

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = active_map_.find(slot_key);
    if (it == active_map_.end() || !it->second.is_active) {
        return false;
    }

    it->second.is_active = false;
    it->second.disconnected_at = std::chrono::steady_clock::now();
    return true;
}

std::optional<HciConnectionRecord> BtHciConnectionTracker::getActiveConnection(
    const std::string& controller, uint16_t handle) const {

    uint16_t valid_handle = handle & 0x0FFF;
    std::string slot_key = makeSlotKey(controller, valid_handle);

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = active_map_.find(slot_key);
    if (it != active_map_.end() && it->second.is_active) {
        return it->second;
    }
    return std::nullopt;
}

std::vector<HciConnectionRecord> BtHciConnectionTracker::getActiveConnections(
    const std::string& controller) const {

    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<HciConnectionRecord> res;
    for (const auto& [k, v] : active_map_) {
        if (v.is_active && v.key.controller_id == controller) {
            res.push_back(v);
        }
    }
    return res;
}

void BtHciConnectionTracker::resetController(const std::string& controller) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto now = std::chrono::steady_clock::now();
    for (auto& [k, v] : active_map_) {
        if (v.key.controller_id == controller && v.is_active) {
            v.is_active = false;
            v.disconnected_at = now;
            // 控制器复位时提前递增代次计数，避免硬件重启后立即重分配同句柄造成串号
            generation_counter_[k]++;
        }
    }
}

size_t BtHciConnectionTracker::syncFromKernel(const std::string& controller) {
    // 1. 打开 HCI 控制套接字 (AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI)
    #ifndef AF_BLUETOOTH
    #define AF_BLUETOOTH 31
    #endif
    #ifndef BTPROTO_HCI
    #define BTPROTO_HCI 1
    #endif
    #ifndef HCIGETCONNLIST
    // _IOR('H', 212, int) = 0x800448d4
    #define HCIGETCONNLIST 0x800448d4
    #endif

    int sock = ::socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
    if (sock < 0) {
        return activeCount();
    }

    // 解析 controller 编号，如 "hci0" -> 0
    uint16_t dev_id = 0;
    if (controller.rfind("hci", 0) == 0 && controller.size() > 3) {
        try {
            dev_id = static_cast<uint16_t>(std::stoi(controller.substr(3)));
        } catch (...) {
            dev_id = 0;
        }
    }

    // 内存缓冲区：头部 4 字节 (dev_id + conn_num) + 最多 64 条 struct hci_conn_info (每条 16 字节)
    struct RawHciConnInfo {
        uint16_t handle;
        uint8_t bdaddr[6];
        uint8_t type;
        uint8_t out;
        uint16_t state;
        uint32_t link_mode;
    } __attribute__((packed));

    uint8_t buf[4 + 64 * sizeof(RawHciConnInfo)] = {0};
    *reinterpret_cast<uint16_t*>(buf) = dev_id;
    *reinterpret_cast<uint16_t*>(buf + 2) = 64; // 最多获取 64 个连接

    int ret = ::ioctl(sock, HCIGETCONNLIST, buf);
    ::close(sock);

    if (ret < 0) {
        return activeCount();
    }

    uint16_t conn_num = *reinterpret_cast<uint16_t*>(buf + 2);
    if (conn_num > 64) conn_num = 64;

    std::set<uint16_t> live_handles;
    const RawHciConnInfo* conns = reinterpret_cast<const RawHciConnInfo*>(buf + 4);

    for (uint16_t i = 0; i < conn_num; ++i) {
        uint16_t handle = conns[i].handle & 0x0FFF;
        live_handles.insert(handle);

        // 内核 bdaddr 反序存储 (b[0] 为低位)
        char mac_buf[18];
        snprintf(mac_buf, sizeof(mac_buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                 conns[i].bdaddr[5], conns[i].bdaddr[4], conns[i].bdaddr[3],
                 conns[i].bdaddr[2], conns[i].bdaddr[1], conns[i].bdaddr[0]);
        std::string mac(mac_buf);

        std::string addr_type = (conns[i].type == 0x80) ? "LE" : "BREDR";

        // 更新/建立真实连接
        onConnectionEstablished(controller, handle, mac, addr_type);
    }

    // 检查并清理已被内核销毁但尚未收到事件的句柄
    std::vector<HciConnectionRecord> current_actives = getActiveConnections(controller);
    for (const auto& rec : current_actives) {
        if (live_handles.find(rec.key.handle) == live_handles.end()) {
            onConnectionDisconnected(controller, rec.key.handle);
        }
    }

    return activeCount();
}

size_t BtHciConnectionTracker::activeCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t count = 0;
    for (const auto& [k, v] : active_map_) {
        if (v.is_active) count++;
    }
    return count;
}

} // namespace weaknet
