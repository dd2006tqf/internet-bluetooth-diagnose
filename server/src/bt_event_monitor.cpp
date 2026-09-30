/**
 * @file bt_event_monitor.cpp
 * @brief 蓝牙设备事件采集器实现
 *
 * 见头文件的设计说明。本文件的关键实现点：
 *   - 逐个 hook 独立挂载、独立降级（attachOne 失败只记状态不中止）
 *   - ringbuf 消费线程用 ring_buffer__poll(200ms) 便于及时响应 stop_
 *   - 内核观测 -> RawBtObservation 的转换集中在一处，便于核对字段语义
 *   - 不在此层做任何原因推断或事件合并（那是 normalizer 的职责）
 */

#include "bt_event_monitor.hpp"
#include "bt_link_quality_tracker.hpp"

#include <bpf/bpf.h>
#include <bpf/libbpf.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <sstream>

#include "logger.hpp"

namespace weaknet_dbus {

namespace {

/// 内核枚举 -> 用户态枚举（值域必须一致，见 bt_events.bpf.c 的 enum 定义）
///
/// **按来源域选择映射函数**：mgmt 来源的 raw_reason 是内核 mgmt 枚举
/// （MGMT_DEV_DISCONN_*），其余来源是 HCI error code。两者取值空间不同，
/// 混用会得出错误原因（见 wireless_event.hpp 的 disconnectReasonFromMgmtCode）。
DisconnectReason mapReasonFromKernel(uint8_t source, bool has_reason, uint8_t raw_reason) {
    if (has_reason) {
        return (source == static_cast<uint8_t>(EvidenceSource::KernelMgmt))
                   ? disconnectReasonFromMgmtCode(raw_reason)
                   : disconnectReasonFromHciCode(raw_reason);
    }
    // hci_conn_timeout 不携带 reason，但它的语义就是链路超时——
    // 这是"来源推断"，作为 supplemental hint 参与归一化（优先级低于 mgmt）。
    if (source == static_cast<uint8_t>(EvidenceSource::KernelHciTimeout)) {
        return DisconnectReason::ConnectionTimeout;
    }
    return DisconnectReason::Unknown;
}

/// 内核接口层的 addr_type -> 用户态 BtAddressType
///
/// 注意：**必须同时看 link_type**。内核在这里用的是 HCI 域取值
/// （0=public, 1=random, 2=public_resolved, 3=random_resolved），
/// 单看 addr_type 无法区分 BR/EDR 与 LE（BR/EDR 连接的该字段也常为 0）。
/// 板端实测踩过：LE Random 设备因只读 addr_type 且缺 link_type 判定，
/// 被误记为 BREDR / LE_PUBLIC。
BtAddressType mapAddressType(uint8_t link_type, uint8_t kernel_addr_type) {
    return btAddressTypeFromKernel(link_type, kernel_addr_type);
}

/// 墙钟毫秒（内核 ringbuf 时间戳是 CLOCK_MONOTONIC，不能直接当事件时间用）
uint64_t softwareTimestampMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

}  // namespace

// ============================================================================
// 构造 / 析构
// ============================================================================

BtEventMonitor::BtEventMonitor(WirelessEventStore* store,
                               BtLinkQualityTracker* tracker,
                               BtNormalizerConfig normalizer_cfg)
    : store_(store), tracker_(tracker), normalizer_(std::move(normalizer_cfg)) {}

BtEventMonitor::~BtEventMonitor() {
    stop();
}

// ============================================================================
// 生命周期
// ============================================================================

bool BtEventMonitor::init(const std::string& bpf_object_path, const std::string& gateway_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (bpf_obj_) {
        last_error_ = "already initialized";
        return true;
    }

    stateSupport_.setState(EbpfMonitorState::Initializing, false);
    bpf_object_path_ = bpf_object_path;
    gateway_id_ = gateway_id;

    bpf_obj_ = bpf_object__open(bpf_object_path.c_str());
    if (!bpf_obj_) {
        last_error_ = "bpf_object__open failed for " + bpf_object_path +
                      ": " + std::string(strerror(errno));
        stateSupport_.setState(EbpfMonitorState::Fallback, false, last_error_);
        LOG_WARNING(LogModule::BLUETOOTH, "BtEventMonitor: " << last_error_);
        return false;
    }

    if (bpf_object__load(bpf_obj_) != 0) {
        last_error_ = "bpf_object__load failed: " + std::string(strerror(errno));
        bpf_object__close(bpf_obj_);
        bpf_obj_ = nullptr;
        stateSupport_.setState(EbpfMonitorState::Fallback, false, last_error_);
        LOG_WARNING(LogModule::BLUETOOTH, "BtEventMonitor: " << last_error_);
        return false;
    }

    // ---- 逐个挂载 hook，各自独立降级 ----
    //
    // 挂点清单与 bt_events.bpf.c 的 SEC() 一一对应。
    // mgmt_device_disconnected 是首选事实来源（内核 mgmt 层统一出口，参数带
    // reason），其余作为补充证据。任一失败不影响其它。
    struct HookSpec {
        const char* kernel_function;
        const char* program_name;
    };
    const HookSpec specs[] = {
        {"mgmt_device_disconnected", "bt_mgmt_device_disconnected"},
        {"hci_disconnect",           "bt_hci_disconnect"},
        {"hci_conn_timeout",         "bt_hci_conn_timeout"},
        {"hci_conn_del",             "bt_hci_conn_del"},
    };

    int attached_count = 0;
    for (const auto& spec : specs) {
        BtHookStatus status = attachOne(spec.kernel_function, spec.program_name);
        if (status.attached) ++attached_count;
        hooks_.push_back(std::move(status));
    }

    if (attached_count == 0) {
        last_error_ = "all kprobe attach attempts failed";
        bpf_object__close(bpf_obj_);
        bpf_obj_ = nullptr;
        stateSupport_.setState(EbpfMonitorState::Fallback, false, last_error_);
        LOG_WARNING(LogModule::BLUETOOTH, "BtEventMonitor: " << last_error_
                    << " — bluetooth event capture degraded to disabled");
        return false;
    }

    // ---- ringbuf ----
    const int rb_map_fd = bpf_object__find_map_fd_by_name(bpf_obj_, "bt_events");
    if (rb_map_fd < 0) {
        last_error_ = "ringbuf map 'bt_events' not found";
        stateSupport_.setState(EbpfMonitorState::Fallback, false, last_error_);
        LOG_WARNING(LogModule::BLUETOOTH, "BtEventMonitor: " << last_error_);
        return false;
    }
    ringbuf_ = ring_buffer__new(rb_map_fd, &BtEventMonitor::onRingbufSample, this, nullptr);
    if (!ringbuf_) {
        last_error_ = "ring_buffer__new failed: " + std::string(strerror(errno));
        stateSupport_.setState(EbpfMonitorState::Fallback, false, last_error_);
        LOG_WARNING(LogModule::BLUETOOTH, "BtEventMonitor: " << last_error_);
        return false;
    }

    cfg_map_fd_ = bpf_object__find_map_fd_by_name(bpf_obj_, "bt_events_cfg");

    stateSupport_.setState(EbpfMonitorState::Attached, true, attachedHooksSummary());
    LOG_INFO(LogModule::BLUETOOTH,
             "BtEventMonitor: attached hooks: " << attachedHooksSummary());
    return true;
}

bool BtEventMonitor::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!bpf_obj_ || !ringbuf_) {
        last_error_ = "start() called before successful init()";
        return false;
    }

    // 打开内核侧开关：init 阶段不统计，start 后才开始上报
    if (cfg_map_fd_ >= 0) {
        uint32_t key = 0;
        uint32_t value = 1;
        bpf_map_update_elem(cfg_map_fd_, &key, &value, BPF_ANY);
    }

    stop_.store(false);
    running_.store(true);
    consumer_ = std::thread(&BtEventMonitor::consumeLoop, this);
    return true;
}

void BtEventMonitor::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_.store(true);
    }
    if (consumer_.joinable()) consumer_.join();

    std::lock_guard<std::mutex> lock(mutex_);
    running_.store(false);

    // 关内核开关，避免 detach 前继续产生观测
    if (cfg_map_fd_ >= 0) {
        uint32_t key = 0;
        uint32_t value = 0;
        bpf_map_update_elem(cfg_map_fd_, &key, &value, BPF_ANY);
    }

    for (bpf_link* link : links_) {
        if (link) bpf_link__destroy(link);
    }
    links_.clear();

    if (ringbuf_) {
        ring_buffer__free(ringbuf_);
        ringbuf_ = nullptr;
    }
    if (bpf_obj_) {
        bpf_object__close(bpf_obj_);
        bpf_obj_ = nullptr;
    }
    cfg_map_fd_ = -1;

    if (!hooks_.empty()) {
        stateSupport_.setState(EbpfMonitorState::Stopped, false, "stopped");
    }
}

// ============================================================================
// 挂载
// ============================================================================

BtHookStatus BtEventMonitor::attachOne(const std::string& kernel_function,
                                       const std::string& program_name) {
    BtHookStatus status;
    status.kernel_function = kernel_function;

    bpf_program* prog = bpf_object__find_program_by_name(bpf_obj_, program_name.c_str());
    if (!prog) {
        status.error = "program not found in BPF object";
        LOG_WARNING(LogModule::BLUETOOTH, "BtEventMonitor: program '" << program_name
                    << "' not found — skipping hook " << kernel_function);
        return status;
    }

    bpf_link* link = bpf_program__attach(prog);
    const long err = libbpf_get_error(link);
    if (err) {
        status.error = "attach failed: " + std::string(strerror(errno));
        LOG_WARNING(LogModule::BLUETOOTH, "BtEventMonitor: attach " << program_name
                    << " -> kprobe/" << kernel_function << " failed: "
                    << strerror(errno) << " — continuing with remaining hooks");
        return status;
    }

    links_.push_back(link);
    status.attached = true;
    LOG_INFO(LogModule::BLUETOOTH, "BtEventMonitor: " << program_name
             << " attached to kprobe/" << kernel_function);
    return status;
}

// ============================================================================
// ringbuf 消费
// ============================================================================

int BtEventMonitor::onRingbufSample(void* ctx, void* data, size_t size) {
    auto* self = static_cast<BtEventMonitor*>(ctx);
    if (!self || !data || size < sizeof(KernelBtObservation)) return 0;
    self->handleObservation(*static_cast<const KernelBtObservation*>(data));
    return 0;
}

void BtEventMonitor::handleObservation(const KernelBtObservation& obs) {
    // 内核观测 -> 用户态原始观测。
    // 注意本函数**不**决定"这是不是一个业务事件"——它只把事实投递给归一化器，
    // 由归一化器按合并键与时间窗决定最终产出几个 canonical 事件。
    RawBtObservation out;
    out.timestamp_ms = softwareTimestampMs();  // 墙钟时间（展示/持久化）
    out.monotonic_ns = obs.timestamp_ns;       // 直接保留 BPF 产生的 bpf_ktime_get_ns()（严防因果倒错）
    out.gateway_id = gateway_id_;
    out.hci_index = obs.hci_index;
    out.device_address = formatBdaddr(obs.bdaddr);
    out.address_type = mapAddressType(obs.link_type, obs.addr_type);
    out.event_type = DeviceEventType::LinkDisconnected;  // 当前全部 hook 都是断连语义
    out.source = static_cast<EvidenceSource>(obs.source);
    out.source_detail = reinterpret_cast<const char*>(obs.source_detail);

    if (obs.has_reason) {
        out.raw_reason_code = obs.reason;   // 原始事实，无损
    }
    out.reason_hint = mapReasonFromKernel(obs.source, obs.has_reason != 0, obs.reason);

    normalizer_.submit(out);
    observations_consumed_.fetch_add(1);
    stateSupport_.recordReadSuccess(0);
}

void BtEventMonitor::consumeLoop() {
    while (!stop_.load()) {
        // 200ms 超时：既保证事件及时上送，又能快速响应 stop_
        const int rc = ring_buffer__poll(ringbuf_, 200);
        if (rc < 0 && rc != -EINTR) {
            stateSupport_.recordReadFailure("ring_buffer__poll failed: " +
                                            std::string(strerror(errno)));
            continue;
        }
        flushToStore(softwareTimestampMs());
    }
    // 退出前把窗口内剩余事件定案，避免丢最后一批
    flushToStore(softwareTimestampMs());
}

void BtEventMonitor::flushToStore(uint64_t now_ms) {
    auto events = normalizer_.flushExpired(now_ms);
    if (events.empty()) return;

    for (auto& ev : events) {
        // Phase 2 核心设计：RSSI 回填在 Normalizer 之后执行（Canonical Enrichment）。
        // 一次断连确定只产生唯一一个 canonical event 并继承最早的单调时间戳后，
        // 严格向前检索断连发生前的最近有效 RSSI（严禁取断开之后的新采样！）。
        if (tracker_ && !ev.rssi_at_event_dbm.has_value() && ev.monotonic_ns > 0) {
            WirelessDeviceKey dev_key;
            dev_key.site_id = ev.site_id;
            dev_key.gateway_id = ev.gateway_id;
            dev_key.hci_index = ev.hci_index;
            dev_key.protocol = ev.protocol;
            dev_key.address_type = ev.address_type;
            dev_key.device_address = ev.device_address;

            auto recent_rssi = tracker_->getRssiBeforeEvent(dev_key, ev.monotonic_ns);
            if (recent_rssi.has_value()) {
                ev.rssi_at_event_dbm = *recent_rssi;
            }
        }

        if (store_) {
            store_->recordEvent(ev);
        }
        events_emitted_.fetch_add(1);
    }
    stateSupport_.recordReadSuccess(0);
}

// ============================================================================
// 状态查询
// ============================================================================

EbpfMonitorState BtEventMonitor::commonState() const {
    return stateSupport_.state();
}

bool BtEventMonitor::isAvailable() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bpf_obj_ != nullptr &&
           stateSupport_.state() == EbpfMonitorState::Attached;
}

std::vector<BtHookStatus> BtEventMonitor::hookStatuses() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return hooks_;
}

std::string BtEventMonitor::attachedHooksSummary() const {
    // 注意：本方法会在 init() 持有 mutex_ 的状态下被调用，
    // 因此**不**加锁，只读 hooks_（init 是 hooks_ 的唯一写入点，串行执行）。
    std::ostringstream oss;
    bool first = true;
    for (const auto& h : hooks_) {
        if (!h.attached) continue;
        if (!first) oss << ", ";
        oss << h.kernel_function;
        first = false;
    }
    return oss.str();
}

std::string BtEventMonitor::lastError() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_error_;
}

std::vector<WirelessDeviceEvent> BtEventMonitor::drainNormalizedEvents() {
    return normalizer_.flushAll();
}

}  // namespace weaknet_dbus
