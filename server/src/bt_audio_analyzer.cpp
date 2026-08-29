// bt_audio_analyzer.cpp
// 蓝牙音频 eBPF 分析器 — 实现文件
//
// 职责：
//   1. 加载 a2dp_media.bpf.o eBPF 程序
//   2. 按优先级探测挂点（kprobe/l2cap_sock_sendmsg → kprobe/l2cap_chan_send）
//   3. 提供 setSessionActive() 控制 eBPF 内核态跟踪开关
//   4. 提供 getStats() 读取内核态累计流量统计
//   5. 挂载失败时记录错误信息，通知上层降级

#include "bt_audio_analyzer.hpp"
#include "logger.hpp"

#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <sstream>
#include <algorithm>
#include <netinet/in.h>

namespace weaknet_dbus {

// ============================================================================
// 构造与析构
// ============================================================================

BtAudioAnalyzer::BtAudioAnalyzer() = default;

BtAudioAnalyzer::~BtAudioAnalyzer() {
    stop();
}

// ============================================================================
// 生命周期管理
// ============================================================================

bool BtAudioAnalyzer::init(const std::string& bpfObjectPath) {
    std::lock_guard<std::mutex> lock(mutex_);

    bpfObjectPath_ = bpfObjectPath;
    state_ = BtAudioAnalyzerState::Uninitialized;
    stateSupport_.setState(EbpfMonitorState::Initializing, false, "loading BPF object");
    lastError_.clear();

    // 1. 打开 BPF 对象文件
    bpfObj_ = bpf_object__open(bpfObjectPath.c_str());
    if (!bpfObj_) {
        lastError_ = "bpf_object__open failed for " + bpfObjectPath + ": " + std::string(strerror(errno));
        LOG_ERROR(LogModule::BLUETOOTH, "BtAudioAnalyzer: " << lastError_);
        state_ = BtAudioAnalyzerState::Error;
        stateSupport_.setState(EbpfMonitorState::Error, false, lastError_);
        return false;
    }

    // 2. 加载 BPF 程序到内核（验证字节码，解析 CO-RE 重定位）
    if (bpf_object__load(bpfObj_) != 0) {
        lastError_ = "bpf_object__load failed: " + std::string(strerror(errno));
        LOG_ERROR(LogModule::BLUETOOTH, "BtAudioAnalyzer: " << lastError_);
        bpf_object__close(bpfObj_);
        bpfObj_ = nullptr;
        state_ = BtAudioAnalyzerState::Error;
        stateSupport_.setState(EbpfMonitorState::Error, false, lastError_);
        return false;
    }

    // 3. 获取 Map 文件描述符
    statsMapFd_ = bpf_object__find_map_fd_by_name(bpfObj_, "bt_traffic");
    sessionsMapFd_ = bpf_object__find_map_fd_by_name(bpfObj_, "active_sessions");
    cfgMapFd_ = bpf_object__find_map_fd_by_name(bpfObj_, "btaudio_cfg");

    if (statsMapFd_ < 0 || sessionsMapFd_ < 0 || cfgMapFd_ < 0) {
        lastError_ = "Failed to find BPF maps";
        LOG_ERROR(LogModule::BLUETOOTH, "BtAudioAnalyzer: " << lastError_ << " (stats="
                  << statsMapFd_ << " sessions=" << sessionsMapFd_ << " cfg=" << cfgMapFd_ << ")");
        bpf_object__close(bpfObj_);
        bpfObj_ = nullptr;
        state_ = BtAudioAnalyzerState::Error;
        stateSupport_.setState(EbpfMonitorState::Error, false, lastError_);
        return false;
    }

    // 4. 按优先级尝试挂载钩子
    // 优先级 1: kprobe/l2cap_sock_sendmsg — 精确定位设备
    link1_ = tryAttachKprobe("l2cap_sock_sendmsg", "l2cap_send_entry");
    if (link1_) {
        attachedHookName_ = "kprobe/l2cap_sock_sendmsg";
        LOG_INFO(LogModule::BLUETOOTH, "BtAudioAnalyzer: attached to " << attachedHookName_);
    } else {
        // 优先级 2: kprobe/__sock_sendmsg — 通用钩子，聚合所有蓝牙流量
        link2_ = tryAttachKprobe("__sock_sendmsg", "sock_sendmsg_entry");
        if (link2_) {
            attachedHookName_ = "kprobe/__sock_sendmsg (aggregated)";
            LOG_INFO(LogModule::BLUETOOTH, "BtAudioAnalyzer: attached to " << attachedHookName_);
        }
    }

    if (!link1_ && !link2_) {
        // 所有挂点均失败，释放资源并降级
        lastError_ = "All kprobe attach attempts failed: l2cap_sock_sendmsg, l2cap_chan_send";
        LOG_WARNING(LogModule::BLUETOOTH, "BtAudioAnalyzer: " << lastError_ << " — falling back to D-Bus-only mode");
        bpf_object__close(bpfObj_);
        bpfObj_ = nullptr;
        statsMapFd_ = -1;
        sessionsMapFd_ = -1;
        cfgMapFd_ = -1;
        state_ = BtAudioAnalyzerState::Fallback;
        stateSupport_.setState(EbpfMonitorState::Fallback, false, lastError_);
        return false;
    }

    // 5. 全局启用 eBPF 跟踪
    setGlobalEnabled(true);

    state_ = BtAudioAnalyzerState::Attached;
    stateSupport_.setState(EbpfMonitorState::Attached, true, "BPF hook attached");
    stateSupport_.recordProbeAttached();
    LOG_INFO(LogModule::BLUETOOTH, "BtAudioAnalyzer: initialization complete, hook=" << attachedHookName_);
    return true;
}

void BtAudioAnalyzer::stop() {
    std::lock_guard<std::mutex> lock(mutex_);

    // 全局禁用
    setGlobalEnabled(false);

    // 销毁链接句柄
    if (link1_) {
        bpf_link__destroy(link1_);
        link1_ = nullptr;
    }
    if (link2_) {
        bpf_link__destroy(link2_);
        link2_ = nullptr;
    }

    // 关闭 BPF 对象
    if (bpfObj_) {
        bpf_object__close(bpfObj_);
        bpfObj_ = nullptr;
    }

    statsMapFd_ = -1;
    sessionsMapFd_ = -1;
    cfgMapFd_ = -1;
    state_ = BtAudioAnalyzerState::Uninitialized;
    stateSupport_.setState(EbpfMonitorState::Stopped, false, "stopped");
    attachedHookName_.clear();
    LOG_INFO(LogModule::BLUETOOTH, "BtAudioAnalyzer: stopped");
}

EbpfMonitorState BtAudioAnalyzer::commonState() const {
    std::lock_guard<std::mutex> lock(mutex_);
    switch (state_) {
        case BtAudioAnalyzerState::Attached: return EbpfMonitorState::Attached;
        case BtAudioAnalyzerState::Fallback: return EbpfMonitorState::Fallback;
        case BtAudioAnalyzerState::Error: return EbpfMonitorState::Error;
        case BtAudioAnalyzerState::Uninitialized: default: return EbpfMonitorState::Uninitialized;
    }
}

bool BtAudioAnalyzer::isAvailable() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == BtAudioAnalyzerState::Attached;
}

std::string BtAudioAnalyzer::attachedHookName() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return attachedHookName_;
}

std::string BtAudioAnalyzer::lastError() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastError_;
}

// ============================================================================
// 会话控制
// ============================================================================

bool BtAudioAnalyzer::setSessionActive(const std::string& mac, bool active, uint8_t direction) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (state_ != BtAudioAnalyzerState::Attached) {
        return false;
    }

    if (sessionsMapFd_ < 0) {
        return false;
    }

    // 解析 MAC 地址
    uint8_t bdaddr[6] = {0};
    if (!parseMac(mac, bdaddr)) {
        LOG_WARNING(LogModule::BLUETOOTH, "BtAudioAnalyzer: invalid MAC address: " << mac);
        return false;
    }

    // 构造 device_key
    uint8_t key_bdaddr[6];
    uint8_t key_dir;
    fillDeviceKey(mac, direction, key_bdaddr, key_dir);

    // 写入 active_sessions map
    struct {
        uint8_t bdaddr[6];
        uint8_t direction;
    } __attribute__((packed)) key = {};
    memcpy(key.bdaddr, bdaddr, 6);
    key.direction = direction;

    struct {
        uint8_t enabled;
        uint8_t reserved[3];
    } __attribute__((packed)) value = {};
    value.enabled = active ? 1 : 0;

    int ret = bpf_map_update_elem(sessionsMapFd_, &key, &value, BPF_ANY);
    if (ret != 0) {
        LOG_WARNING(LogModule::BLUETOOTH, "BtAudioAnalyzer: bpf_map_update_elem(active_sessions) failed for "
                    << mac << " (errno=" << errno << ")");
        return false;
    }

    return true;
}

void BtAudioAnalyzer::setGlobalEnabled(bool enabled) {
    if (cfgMapFd_ >= 0) {
        uint32_t key = 0;
        struct {
            uint8_t enabled;
            uint8_t reserved[3];
        } __attribute__((packed)) value = {};
        value.enabled = enabled ? 1 : 0;
        bpf_map_update_elem(cfgMapFd_, &key, &value, BPF_ANY);
    }
}

// ============================================================================
// 数据读取
// ============================================================================

bool BtAudioAnalyzer::getStats(const std::string& mac, uint8_t direction, BtTrafficStats* out) const {
    if (!out) return false;

    std::lock_guard<std::mutex> lock(mutex_);

    if (state_ != BtAudioAnalyzerState::Attached) {
        return false;
    }

    uint8_t bdaddr[6];
    if (!parseMac(mac, bdaddr)) {
        return false;
    }

    auto started = std::chrono::steady_clock::now();
    bool ok = readStatsFromMap(bdaddr, direction, out);
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started).count();
    if (ok) {
        stateSupport_.recordReadSuccess(static_cast<uint64_t>(elapsed));
    } else {
        stateSupport_.recordReadFailure("bt_traffic map lookup failed");
    }
    return ok;
}

std::vector<std::pair<std::string, BtTrafficStats>> BtAudioAnalyzer::getAllStats() const {
    std::vector<std::pair<std::string, BtTrafficStats>> result;
    std::lock_guard<std::mutex> lock(mutex_);

    if (state_ != BtAudioAnalyzerState::Attached || statsMapFd_ < 0) {
        return result;
    }

    // 遍历 bt_traffic map

    // 第一次遍历：获取第一个 key
    struct {
        uint8_t bdaddr[6];
        uint8_t direction;
        uint8_t padding;
    } __attribute__((packed)) key = {};
    struct {
        uint8_t bdaddr[6];
        uint8_t direction;
        uint8_t padding;
    } __attribute__((packed)) next_key = {};

    int ret = bpf_map_get_next_key(statsMapFd_, nullptr, &next_key);
    while (ret == 0) {
        memcpy(&key, &next_key, sizeof(key));

        BtTrafficStats stats;
        if (readStatsFromMap(key.bdaddr, key.direction, &stats)) {
            // 将 BDADDR 转为 MAC 字符串
            char macBuf[18];
            snprintf(macBuf, sizeof(macBuf), "%02X:%02X:%02X:%02X:%02X:%02X",
                     key.bdaddr[0], key.bdaddr[1], key.bdaddr[2],
                     key.bdaddr[3], key.bdaddr[4], key.bdaddr[5]);
            result.emplace_back(std::string(macBuf), stats);
        }

        memcpy(&next_key, &key, sizeof(key));
        ret = bpf_map_get_next_key(statsMapFd_, &next_key, &next_key);
    }

    return result;
}

void BtAudioAnalyzer::clearDeviceStats(const std::string& mac) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (state_ != BtAudioAnalyzerState::Attached || statsMapFd_ < 0 || sessionsMapFd_ < 0) {
        return;
    }

    uint8_t bdaddr[6];
    if (!parseMac(mac, bdaddr)) {
        return;
    }

    // 清理两个方向的统计
    for (uint8_t dir = 0; dir <= 1; dir++) {
        struct {
            uint8_t bdaddr[6];
            uint8_t direction;
        } __attribute__((packed)) key = {};
        memcpy(key.bdaddr, bdaddr, 6);
        key.direction = dir;

        bpf_map_delete_elem(statsMapFd_, &key);
        bpf_map_delete_elem(sessionsMapFd_, &key);
    }
}

// ============================================================================
// 内部方法
// ============================================================================

bool BtAudioAnalyzer::parseMac(const std::string& mac, uint8_t bdaddr[6]) {
    // 预期格式: "AA:BB:CC:DD:EE:FF"
    if (mac.length() != 17) return false;

    unsigned int bytes[6];
    int parsed = sscanf(mac.c_str(), "%02X:%02X:%02X:%02X:%02X:%02X",
                        &bytes[0], &bytes[1], &bytes[2],
                        &bytes[3], &bytes[4], &bytes[5]);
    if (parsed != 6) return false;

    for (int i = 0; i < 6; i++) {
        if (bytes[i] > 255) return false;
        bdaddr[i] = static_cast<uint8_t>(bytes[i]);
    }
    return true;
}

void BtAudioAnalyzer::fillDeviceKey(const std::string& mac, uint8_t direction,
                                     uint8_t key_bdaddr[6], uint8_t& key_dir) {
    parseMac(mac, key_bdaddr);
    key_dir = direction;
}

bpf_link* BtAudioAnalyzer::tryAttachKprobe(const std::string& funcName, const std::string& progName) {
    if (!bpfObj_) return nullptr;

    // 查找 BPF 程序
    bpf_program* prog = bpf_object__find_program_by_name(bpfObj_, progName.c_str());
    if (!prog) {
        LOG_WARNING(LogModule::BLUETOOTH, "BtAudioAnalyzer: program '" << progName
                    << "' not found in BPF object");
        return nullptr;
    }

    // 尝试挂载
    bpf_link* link = bpf_program__attach(prog);
    long err = libbpf_get_error(link);
    if (err) {
        LOG_WARNING(LogModule::BLUETOOTH, "BtAudioAnalyzer: attach " << progName
                    << " → kprobe/" << funcName << " failed: " << err
                    << " (errno=" << errno << " " << strerror(errno) << ")");
        return nullptr;
    }

    LOG_INFO(LogModule::BLUETOOTH, "BtAudioAnalyzer: " << progName << " attached to kprobe/" << funcName);
    return link;
}

bool BtAudioAnalyzer::readStatsFromMap(const uint8_t bdaddr[6], uint8_t direction,
                                        BtTrafficStats* out) const {
    if (statsMapFd_ < 0 || !out) return false;

    struct {
        uint8_t bdaddr[6];
        uint8_t direction;
    } __attribute__((packed)) key = {};
    memcpy(key.bdaddr, bdaddr, 6);
    key.direction = direction;

    // 从内核 map 读取统计值
    struct {
        uint64_t bytes;
        uint64_t packets;
        uint64_t last_packet_ns;
        uint64_t gap_count;
        uint64_t max_gap_ns;
    } __attribute__((packed)) value = {};

    if (bpf_map_lookup_elem(statsMapFd_, &key, &value) != 0) {
        return false;  // 该设备尚未产生任何流量
    }

    out->bytes = value.bytes;
    out->packets = value.packets;
    out->lastPacketNs = value.last_packet_ns;
    out->gapCount = value.gap_count;
    out->maxGapNs = value.max_gap_ns;
    return true;
}

}  // namespace weaknet_dbus