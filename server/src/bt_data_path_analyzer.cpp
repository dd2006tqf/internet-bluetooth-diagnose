#include "bt_data_path_analyzer.hpp"
#include "logger.hpp"
#include "utils/json_escape.hpp"

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <cstring>
#include <sstream>
#include <algorithm>

namespace weaknet_dbus {

std::string BtDataPathObservation::toJson() const {
    std::ostringstream oss;
    oss << "{"
        << "\"observation_type\":\"" << observation_type << "\","
        << "\"controller_id\":\"" << weaknet_utils::escapeJsonString(controller_id) << "\","
        << "\"connection_handle\":" << connection_handle << ","
        << "\"connection_generation\":" << connection_generation << ","
        << "\"attribution_scope\":\"" << attribution_scope << "\",";
    if (device_id.has_value()) {
        oss << "\"device_id\":\"" << weaknet_utils::escapeJsonString(*device_id) << "\",";
    } else {
        oss << "\"device_id\":null,";
    }
    oss << "\"window_ms\":" << window_ms << ","
        << "\"tx_bytes\":" << tx_bytes << ","
        << "\"rx_bytes\":" << rx_bytes << ","
        << "\"tx_packets\":" << tx_packets << ","
        << "\"rx_packets\":" << rx_packets << ","
        << "\"tx_throughput_bps\":" << tx_throughput_bps << ","
        << "\"rx_throughput_bps\":" << rx_throughput_bps << ","
        << "\"max_gap_ms\":" << max_gap_ms << ","
        << "\"status\":\"" << status << "\""
        << "}";
    return oss.str();
}

BtDataPathAnalyzer::BtDataPathAnalyzer(std::shared_ptr<weaknet::BtHciConnectionTracker> tracker)
    : tracker_(tracker ? std::move(tracker) : std::make_shared<weaknet::BtHciConnectionTracker>()) {}

BtDataPathAnalyzer::~BtDataPathAnalyzer() {
    stop();
}

bool BtDataPathAnalyzer::init(const std::string& bpfObjectPath, const std::string& controller) {
    std::lock_guard<std::mutex> lock(mutex_);
    controller_id_ = controller.empty() ? "hci0" : controller;
    bpfObjectPath_ = bpfObjectPath;
    lastError_.clear();
    state_ = EbpfMonitorState::Initializing;
    stateSupport_.setState(EbpfMonitorState::Initializing, false, "loading bt_hci_traffic BPF object");

    bpfObj_ = bpf_object__open(bpfObjectPath.c_str());
    if (!bpfObj_) {
        lastError_ = "bpf_object__open failed for " + bpfObjectPath + ": " + std::string(strerror(errno));
        LOG_WARNING(LogModule::BLUETOOTH, "BtDataPathAnalyzer: " << lastError_ << " — fallback mode");
        state_ = EbpfMonitorState::Fallback;
        stateSupport_.setState(EbpfMonitorState::Fallback, false, lastError_);
        return false;
    }

    if (bpf_object__load(bpfObj_) != 0) {
        lastError_ = "bpf_object__load failed: " + std::string(strerror(errno));
        LOG_WARNING(LogModule::BLUETOOTH, "BtDataPathAnalyzer: " << lastError_ << " — fallback mode");
        bpf_object__close(bpfObj_);
        bpfObj_ = nullptr;
        state_ = EbpfMonitorState::Fallback;
        stateSupport_.setState(EbpfMonitorState::Fallback, false, lastError_);
        return false;
    }

    mapFd_ = bpf_object__find_map_fd_by_name(bpfObj_, "bt_acl_traffic");
    globalMapFd_ = bpf_object__find_map_fd_by_name(bpfObj_, "bt_acl_global_stats");

    if (mapFd_ < 0) {
        lastError_ = "Failed to find 'bt_acl_traffic' map in BPF object";
        LOG_WARNING(LogModule::BLUETOOTH, "BtDataPathAnalyzer: " << lastError_);
        bpf_object__close(bpfObj_);
        bpfObj_ = nullptr;
        state_ = EbpfMonitorState::Fallback;
        stateSupport_.setState(EbpfMonitorState::Fallback, false, lastError_);
        return false;
    }

    // 挂载 TX 与 RX kprobe 探针
    bpf_program* prog_tx = bpf_object__find_program_by_name(bpfObj_, "bt_hci_send_frame");
    bpf_program* prog_rx = bpf_object__find_program_by_name(bpfObj_, "bt_hci_recv_frame");

    int attached_count = 0;
    std::string hooks;

    if (prog_tx) {
        link_tx_ = bpf_program__attach(prog_tx);
        if (!libbpf_get_error(link_tx_)) {
            attached_count++;
            hooks += "kprobe/hci_send_frame";
        } else {
            link_tx_ = nullptr;
        }
    }

    if (prog_rx) {
        link_rx_ = bpf_program__attach(prog_rx);
        if (!libbpf_get_error(link_rx_)) {
            attached_count++;
            if (!hooks.empty()) hooks += ",";
            hooks += "kprobe/hci_recv_frame";
        } else {
            link_rx_ = nullptr;
        }
    }

    if (attached_count == 0) {
        lastError_ = "All kprobe attach attempts failed (hci_send_frame, hci_recv_frame)";
        LOG_WARNING(LogModule::BLUETOOTH, "BtDataPathAnalyzer: " << lastError_ << " — fallback mode");
        bpf_object__close(bpfObj_);
        bpfObj_ = nullptr;
        mapFd_ = -1;
        globalMapFd_ = -1;
        state_ = EbpfMonitorState::Fallback;
        stateSupport_.setState(EbpfMonitorState::Fallback, false, lastError_);
        return false;
    }

    attachedHookName_ = hooks;
    state_ = EbpfMonitorState::Attached;
    stateSupport_.setState(EbpfMonitorState::Attached, true, attachedHookName_);
    LOG_INFO(LogModule::BLUETOOTH, "BtDataPathAnalyzer attached successfully: " << attachedHookName_);
    return true;
}

void BtDataPathAnalyzer::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (link_tx_) {
        bpf_link__destroy(link_tx_);
        link_tx_ = nullptr;
    }
    if (link_rx_) {
        bpf_link__destroy(link_rx_);
        link_rx_ = nullptr;
    }
    if (bpfObj_) {
        bpf_object__close(bpfObj_);
        bpfObj_ = nullptr;
    }
    mapFd_ = -1;
    globalMapFd_ = -1;
    state_ = EbpfMonitorState::Stopped;
    stateSupport_.setState(EbpfMonitorState::Stopped, false, "stopped");
}

EbpfMonitorState BtDataPathAnalyzer::commonState() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

bool BtDataPathAnalyzer::isAvailable() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == EbpfMonitorState::Attached;
}

std::string BtDataPathAnalyzer::attachedHookName() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return attachedHookName_;
}

std::string BtDataPathAnalyzer::lastError() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastError_;
}

bool BtDataPathAnalyzer::readRawMapValue(uint16_t handle, RawAclTrafficVal* out) const {
    if (!out) return false;
    uint32_t raw_h = handle & 0x0FFF;

    // 解析控制器序号 (hci0 -> 0)
    uint32_t hci_id = 0;
    if (controller_id_.rfind("hci", 0) == 0 && controller_id_.size() > 3) {
        try {
            hci_id = static_cast<uint32_t>(std::stoi(controller_id_.substr(3)));
        } catch (...) {
            hci_id = 0;
        }
    }
    uint32_t map_key = (hci_id << 16) | raw_h;

    if (mapFd_ >= 0) {
        int ret = bpf_map_lookup_elem(mapFd_, &map_key, out);
        if (ret != 0 && hci_id == 0) {
            // 兼容可能单以 handle 为 key 的老版本 Map 布局
            ret = bpf_map_lookup_elem(mapFd_, &raw_h, out);
        }
        return (ret == 0);
    }

    // 内存测试桩回退 (优先复合 key，后退回 raw handle)
    auto it = mock_map_.find(map_key);
    if (it != mock_map_.end()) {
        *out = it->second;
        return true;
    }
    auto it_raw = mock_map_.find(raw_h);
    if (it_raw != mock_map_.end()) {
        *out = it_raw->second;
        return true;
    }
    return false;
}

void BtDataPathAnalyzer::injectMockSample(uint16_t handle, const RawAclTrafficVal& val) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint32_t raw_h = handle & 0x0FFF;
    uint32_t hci_id = 0;
    if (controller_id_.rfind("hci", 0) == 0 && controller_id_.size() > 3) {
        try {
            hci_id = static_cast<uint32_t>(std::stoi(controller_id_.substr(3)));
        } catch (...) {
            hci_id = 0;
        }
    }
    uint32_t map_key = (hci_id << 16) | raw_h;
    mock_map_[map_key] = val;
    mock_map_[raw_h] = val; // 兼容
}

size_t BtDataPathAnalyzer::pruneStaleEntries(const std::set<uint16_t>& live_handles) {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t pruned = 0;

    // 内核 Map 条目遍历（key 可能是复合 key，取低 12 位与活跃集比对）
    if (mapFd_ >= 0) {
        std::vector<uint32_t> dead_keys;
        uint32_t key = 0, next_key = 0;
        int ret = bpf_map_get_next_key(mapFd_, nullptr, &next_key);
        while (ret == 0) {
            key = next_key;
            uint16_t h = static_cast<uint16_t>(key & 0x0FFF);
            if (live_handles.find(h) == live_handles.end()) {
                dead_keys.push_back(key);
            }
            ret = bpf_map_get_next_key(mapFd_, &key, &next_key);
        }
        for (uint32_t k : dead_keys) {
            if (bpf_map_delete_elem(mapFd_, &k) == 0) {
                pruned++;
                last_samples_.erase(static_cast<uint16_t>(k & 0x0FFF));
            }
        }
    }

    // 内存测试桩同步清理
    for (auto it = mock_map_.begin(); it != mock_map_.end(); ) {
        uint16_t h = static_cast<uint16_t>(it->first & 0x0FFF);
        if (live_handles.find(h) == live_handles.end()) {
            last_samples_.erase(h);
            it = mock_map_.erase(it);
            pruned++;
        } else {
            ++it;
        }
    }

    if (pruned > 0) {
        LOG_DEBUG(LogModule::BLUETOOTH, "BtDataPathAnalyzer pruned " << pruned
                  << " stale map entries (dead handles)");
    }
    return pruned;
}

std::vector<BtDataPathObservation> BtDataPathAnalyzer::collectObservations(
    std::chrono::steady_clock::time_point now_point) {

    std::vector<BtDataPathObservation> observations;
    std::lock_guard<std::mutex> lock(mutex_);

    // 1. 搜集当前存在的所有 Handle
    std::vector<uint32_t> handles;

    if (mapFd_ >= 0) {
        uint32_t key = 0;
        uint32_t next_key = 0;
        int ret = bpf_map_get_next_key(mapFd_, nullptr, &next_key);
        while (ret == 0) {
            key = next_key;
            handles.push_back(key);
            ret = bpf_map_get_next_key(mapFd_, &key, &next_key);
        }
    } else {
        for (const auto& [h, _] : mock_map_) {
            handles.push_back(h);
        }
    }

    // 同时补充 tracker 中当前活跃但内核 map 尚未建立条目的连接（保证 0 流量也观测到）
    if (tracker_) {
        auto active_conns = tracker_->getActiveConnections(controller_id_);
        for (const auto& conn : active_conns) {
            uint32_t h = conn.key.handle & 0x0FFF;
            if (std::find(handles.begin(), handles.end(), h) == handles.end()) {
                handles.push_back(h);
            }
        }
    }

    // 2. 逐 Handle 提取、核算滑动增量与速率
    for (uint32_t raw_h : handles) {
        uint16_t h = static_cast<uint16_t>(raw_h & 0x0FFF);
        RawAclTrafficVal curr_val{};
        readRawMapValue(h, &curr_val);

        // 查找 Tracker 身份与代次
        uint32_t current_generation = 0;
        std::string attribution_scope = "unknown";
        std::optional<std::string> device_id = std::nullopt;

        if (tracker_) {
            auto conn_rec = tracker_->getActiveConnection(controller_id_, h);
            if (conn_rec.has_value()) {
                current_generation = conn_rec->key.generation;
                attribution_scope = conn_rec->attribution_scope;
                if (!conn_rec->device_mac.empty()) {
                    device_id = conn_rec->device_mac;
                }
            } else {
                // 未知具体连接归属，但属于该 controller
                attribution_scope = "controller";
            }
        }

        // 增量计算
        uint64_t tx_bytes = 0;
        uint64_t rx_bytes = 0;
        uint64_t tx_pkts = 0;
        uint64_t rx_pkts = 0;
        uint32_t window_ms = 0;
        uint32_t max_gap_ms = 0;

        auto last_it = last_samples_.find(h);
        if (last_it != last_samples_.end()) {
            // 防复用检查：若代次变化或累计计数回绕（如重启），重置增量基准
            if (last_it->second.generation == current_generation &&
                curr_val.tx_bytes >= last_it->second.val.tx_bytes &&
                curr_val.rx_bytes >= last_it->second.val.rx_bytes) {

                tx_bytes = curr_val.tx_bytes - last_it->second.val.tx_bytes;
                rx_bytes = curr_val.rx_bytes - last_it->second.val.rx_bytes;
                tx_pkts = curr_val.tx_packets - last_it->second.val.tx_packets;
                rx_pkts = curr_val.rx_packets - last_it->second.val.rx_packets;

                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now_point - last_it->second.time).count();
                window_ms = (elapsed > 0) ? static_cast<uint32_t>(elapsed) : 1;
            } else {
                // 代次跃迁（复用 Handle 或重连）：
                // 核心防串号逻辑：旧代次遗留的内核累积计数在此代次被设为 Base 基准，
                // 不将历史残留值当作当前新代次的增量，初次跃迁增量归零，彻底杜绝数据串号。
                tx_bytes = 0;
                rx_bytes = 0;
                tx_pkts = 0;
                rx_pkts = 0;
                window_ms = 1000;
            }
        } else {
            // 首次观测此 Handle：若为首代启动，使用当前值；若当前已为高代次（表明进程重启或晚于建立），
            // 设当前内核值为基准，以 0 起步避免把历史值计入本代次首个窗口
            if (current_generation <= 1) {
                tx_bytes = curr_val.tx_bytes;
                rx_bytes = curr_val.rx_bytes;
                tx_pkts = curr_val.tx_packets;
                rx_pkts = curr_val.rx_packets;
            } else {
                tx_bytes = 0;
                rx_bytes = 0;
                tx_pkts = 0;
                rx_pkts = 0;
            }
            window_ms = 1000;
        }

        // 吞吐率计算（bits/s，ACL payload）
        double tx_bps = 0.0;
        double rx_bps = 0.0;
        if (window_ms > 0) {
            tx_bps = (static_cast<double>(tx_bytes) * 8.0 * 1000.0) / window_ms;
            rx_bps = (static_cast<double>(rx_bytes) * 8.0 * 1000.0) / window_ms;
        }

        // 计算最后一次收发时间差（gap）
        if (curr_val.last_tx_ktime_ns > 0 && curr_val.last_rx_ktime_ns > 0) {
            uint64_t diff_ns = (curr_val.last_tx_ktime_ns > curr_val.last_rx_ktime_ns)
                ? (curr_val.last_tx_ktime_ns - curr_val.last_rx_ktime_ns)
                : (curr_val.last_rx_ktime_ns - curr_val.last_tx_ktime_ns);
            max_gap_ms = static_cast<uint32_t>(diff_ns / 1000000ULL);
        }

        // 构造纯净 L1 DTO
        BtDataPathObservation obs;
        obs.observation_type = "BT_HCI_ACL_TRAFFIC";
        obs.controller_id = controller_id_;
        obs.connection_handle = h;
        obs.connection_generation = current_generation;
        obs.attribution_scope = attribution_scope;
        obs.device_id = device_id;
        obs.window_ms = window_ms;
        obs.tx_bytes = tx_bytes;
        obs.rx_bytes = rx_bytes;
        obs.tx_packets = tx_pkts;
        obs.rx_packets = rx_pkts;
        obs.tx_throughput_bps = tx_bps;
        obs.rx_throughput_bps = rx_bps;
        obs.max_gap_ms = max_gap_ms;
        obs.status = "OBSERVED";
        obs.last_tx_ktime_ns = curr_val.last_tx_ktime_ns;
        obs.last_rx_ktime_ns = curr_val.last_rx_ktime_ns;

        observations.push_back(std::move(obs));

        // 更新历史快照
        last_samples_[h] = LastSample{curr_val, now_point, current_generation};
    }

    stateSupport_.recordReadSuccess(0, !observations.empty());
    return observations;
}

} // namespace weaknet_dbus
