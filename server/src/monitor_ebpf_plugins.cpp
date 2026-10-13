/**
 * @file monitor_ebpf_plugins.cpp
 * @brief 内置 eBPF 监控器插件实现
 *
 * 阶段二将 eBPF 对象所有权迁移到对应插件；ServerContext 仅保留查询用
 * non-owning 指针，不改变 BPF 程序、map 或指标算法。
 * order 设计：
 *   10: dns / wifi_loss / http_latency（无共享依赖）
 *   10: traffic（flow_rate 持有者，已在传统组注册）
 *   20: process_profiler（依赖 traffic 的 flow_rate）
 *   20: tcp_retrans / tcp_conn（独立 eBPF 对象）
 */

#include "monitor_registry.hpp"
#include <chrono>
#include <thread>
#include <unistd.h>   // gethostname（bt_events 插件的 gateway_id 兜底）
#include "logger.hpp"

#include "server.hpp"
#include "dns_monitor.hpp"
#include "wifi_packet_loss_monitor.hpp"
#include "http_latency_monitor.hpp"
#include "process_net_profiler.hpp"
#include "tcp_retransmit_monitor.hpp"
#include "tcp_conn_monitor.hpp"
#include "skb_drop_monitor.hpp"
#include "tcp_connect_monitor.hpp"
#include "tcp_rtt_monitor.hpp"
#include "bt_event_monitor.hpp"
#include "bt_data_path_analyzer.hpp"
#include "bt_hci_connection_tracker.hpp"
#include "bt_link_quality_tracker.hpp"
#include "site_incident_correlator.hpp"
#include "wireless_event_store.hpp"
#include "database_manager.hpp"
#include "bt_data_path_anomaly_detector.hpp"
#include "utils/bpf_map_sizing.hpp"

namespace weaknet_dbus {

// ---------------------------------------------------------------------------
// DNS 监控（order 10）
// ---------------------------------------------------------------------------
class DnsPlugin : public IMonitorPlugin {
    ServerContext* ctx_ = nullptr;
    std::thread worker_;
    std::unique_ptr<DnsMonitor> monitor_;
public:
    const char* name() const override { return "dns"; }
    int order() const override { return 10; }
    bool init(ServerContext* ctx) override { ctx_ = ctx; return true; }
    bool start(ServerContext* ctx) override {
        if (!ctx->cfg.dns.enabled.load()) {
            LOG_INFO(LogModule::NETWORK, "DNS monitor disabled by config");
            return true;
        }
        monitor_ = std::make_unique<DnsMonitor>();
        if (!monitor_->init(ctx->cfg.dns.bpf_obj.get().c_str(), ctx->cfg.dns.capture_pages.load(),
                             weaknet::resolveScopePlan(weaknet::MapSizingScope::Dns,
                                 {ctx->cfg.dns.map_sizing.mode.get(), ctx->cfg.dns.map_sizing.entries.load(),
                                  ctx->cfg.dns.map_sizing.ram_budget_bp.load()}, weaknet::totalPhysicalRamBytes()))) {
            monitor_.reset();
            return false;
        }
        {
            std::unique_lock<std::shared_mutex> lock(ctx->monitor_pointers_mutex);
            ctx->dns_monitor = monitor_.get();
        }
        ctx->dns_stop.store(false);
        start_dns_monitor_thread(ctx, &worker_, monitor_.get());
        return true;
    }
    void stop() override {
        if (!ctx_) return;
        ctx_->dns_stop.store(true);
        if (worker_.joinable()) worker_.join();
        {
            std::unique_lock<std::shared_mutex> lock(ctx_->monitor_pointers_mutex);
            if (ctx_->dns_monitor == monitor_.get()) ctx_->dns_monitor = nullptr;
            monitor_.reset();
        }
    }
};

// ---------------------------------------------------------------------------
// Wi-Fi 丢包归因（order 10）
// ---------------------------------------------------------------------------
class WifiLossPlugin : public IMonitorPlugin {
    ServerContext* ctx_ = nullptr;
    std::thread worker_;
    std::unique_ptr<WifiPacketLossMonitor> monitor_;
public:
    const char* name() const override { return "wifi_loss"; }
    int order() const override { return 10; }
    bool init(ServerContext* ctx) override { ctx_ = ctx; return true; }
    bool start(ServerContext* ctx) override {
        if (!ctx->cfg.wifi_loss.enabled.load()) {
            LOG_INFO(LogModule::NETWORK, "Wi-Fi loss monitor disabled by config");
            return true;
        }
        monitor_ = std::make_unique<WifiPacketLossMonitor>();
        if (!monitor_->init(ctx->cfg.wifi_loss.bpf_obj.get().c_str())) {
            monitor_.reset();
            return false;
        }
        {
            std::unique_lock<std::shared_mutex> lock(ctx->monitor_pointers_mutex);
            ctx->wifi_loss_monitor = monitor_.get();
        }
        ctx->wifi_loss_stop.store(false);
        start_wifi_loss_monitor_thread(ctx, &worker_, monitor_.get());
        return true;
    }
    void stop() override {
        if (!ctx_) return;
        ctx_->wifi_loss_stop.store(true);
        if (worker_.joinable()) worker_.join();
        {
            std::unique_lock<std::shared_mutex> lock(ctx_->monitor_pointers_mutex);
            if (ctx_->wifi_loss_monitor == monitor_.get()) ctx_->wifi_loss_monitor = nullptr;
            monitor_.reset();
        }
    }
};

// ---------------------------------------------------------------------------
// HTTP 延迟监控（order 10）
// ---------------------------------------------------------------------------
class HttpLatencyPlugin : public IMonitorPlugin {
    ServerContext* ctx_ = nullptr;
    std::thread worker_;
    std::unique_ptr<HttpLatencyMonitor> monitor_;
public:
    const char* name() const override { return "http_latency"; }
    int order() const override { return 10; }
    bool init(ServerContext* ctx) override { ctx_ = ctx; return true; }
    bool start(ServerContext* ctx) override {
        if (!ctx->cfg.http_latency.enabled.load()) {
            LOG_INFO(LogModule::NETWORK, "HTTP latency monitor disabled by config");
            return true;
        }
        monitor_ = std::make_unique<HttpLatencyMonitor>();
        if (!monitor_->init(ctx->cfg.http_latency.bpf_obj.get().c_str(),
                              weaknet::resolveScopePlan(weaknet::MapSizingScope::HttpLatency,
                                 {ctx->cfg.http_latency.map_sizing.mode.get(), ctx->cfg.http_latency.map_sizing.entries.load(),
                                  ctx->cfg.http_latency.map_sizing.ram_budget_bp.load()}, weaknet::totalPhysicalRamBytes()))) {
            monitor_.reset();
            return false;
        }
        {
            std::unique_lock<std::shared_mutex> lock(ctx->monitor_pointers_mutex);
            ctx->http_latency_monitor = monitor_.get();
        }
        ctx->http_latency_stop.store(false);
        start_http_latency_monitor_thread(ctx, &worker_, monitor_.get());
        return true;
    }
    void stop() override {
        if (!ctx_) return;
        ctx_->http_latency_stop.store(true);
        if (worker_.joinable()) worker_.join();
        {
            std::unique_lock<std::shared_mutex> lock(ctx_->monitor_pointers_mutex);
            if (ctx_->http_latency_monitor == monitor_.get()) ctx_->http_latency_monitor = nullptr;
            monitor_.reset();
        }
    }
};

// ---------------------------------------------------------------------------
// 进程网络画像（order 20：依赖 traffic 的 flow_rate，需在 traffic 之后启动）
// ---------------------------------------------------------------------------
class ProcessProfilerPlugin : public IMonitorPlugin {
    ServerContext* ctx_ = nullptr;
    std::thread worker_;
    std::unique_ptr<ProcessNetProfiler> monitor_;
public:
    const char* name() const override { return "process_profiler"; }
    int order() const override { return 20; }
    std::vector<std::string> dependencies() const override { return {"traffic"}; }
    bool init(ServerContext* ctx) override { ctx_ = ctx; return true; }
    bool start(ServerContext* ctx) override {
        if (!ctx->cfg.process_profiler.enabled.load()) {
            LOG_INFO(LogModule::NETWORK, "Process net profiler disabled by config");
            return true;
        }
        monitor_ = std::make_unique<ProcessNetProfiler>();
        if (!monitor_->init(ctx->cfg.process_profiler.bpf_obj.get().c_str(),
                              weaknet::resolveScopePlan(weaknet::MapSizingScope::ProcessProfiler,
                                 {ctx->cfg.process_profiler.map_sizing.mode.get(), ctx->cfg.process_profiler.map_sizing.entries.load(),
                                  ctx->cfg.process_profiler.map_sizing.ram_budget_bp.load()}, weaknet::totalPhysicalRamBytes()))) {
            monitor_.reset();
            return false;
        }
        {
            std::unique_lock<std::shared_mutex> lock(ctx->monitor_pointers_mutex);
            ctx->process_net_profiler = monitor_.get();
        }
        ctx->process_profiler_stop.store(false);
        start_process_net_profiler_thread(ctx, &worker_, monitor_.get());
        return true;
    }
    void stop() override {
        if (!ctx_) return;
        ctx_->process_profiler_stop.store(true);
        if (worker_.joinable()) worker_.join();
        {
            std::unique_lock<std::shared_mutex> lock(ctx_->monitor_pointers_mutex);
            if (ctx_->process_net_profiler == monitor_.get()) ctx_->process_net_profiler = nullptr;
            monitor_.reset();
        }
    }
};

// ---------------------------------------------------------------------------
// TCP 重传监控（order 20）
// ---------------------------------------------------------------------------
class TcpRetransPlugin : public IMonitorPlugin {
    ServerContext* ctx_ = nullptr;
    std::thread worker_;
    std::unique_ptr<TcpRetransMonitor> monitor_;
public:
    const char* name() const override { return "tcp_retrans"; }
    int order() const override { return 20; }
    bool init(ServerContext* ctx) override { ctx_ = ctx; return true; }
    bool start(ServerContext* ctx) override {
        if (!ctx->cfg.tcp_retrans.enabled.load()) {
            LOG_INFO(LogModule::NETWORK, "TCP retransmit monitor disabled by config");
            return true;
        }
        monitor_ = std::make_unique<TcpRetransMonitor>();
        if (!monitor_->init(ctx->cfg.tcp_retrans.bpf_obj.get().c_str(),
                              weaknet::resolveScopePlan(weaknet::MapSizingScope::TcpRetrans,
                                 {ctx->cfg.tcp_retrans.map_sizing.mode.get(), ctx->cfg.tcp_retrans.map_sizing.entries.load(),
                                  ctx->cfg.tcp_retrans.map_sizing.ram_budget_bp.load()}, weaknet::totalPhysicalRamBytes()))) {
            monitor_.reset();
            return false;
        }
        {
            std::unique_lock<std::shared_mutex> lock(ctx->monitor_pointers_mutex);
            ctx->tcp_retrans_monitor = monitor_.get();
        }
        ctx->tcp_retrans_stop.store(false);
        start_tcp_retrans_monitor_thread(ctx, &worker_, monitor_.get());
        return true;
    }
    void stop() override {
        if (!ctx_) return;
        ctx_->tcp_retrans_stop.store(true);
        if (worker_.joinable()) worker_.join();
        {
            std::unique_lock<std::shared_mutex> lock(ctx_->monitor_pointers_mutex);
            if (ctx_->tcp_retrans_monitor == monitor_.get()) ctx_->tcp_retrans_monitor = nullptr;
            monitor_.reset();
        }
    }
};

// ---------------------------------------------------------------------------
// TCP 连接生命周期（order 20）
// ---------------------------------------------------------------------------
class TcpConnPlugin : public IMonitorPlugin {
    ServerContext* ctx_ = nullptr;
    std::thread worker_;
    std::unique_ptr<TcpConnMonitor> monitor_;
public:
    const char* name() const override { return "tcp_conn"; }
    int order() const override { return 20; }
    bool init(ServerContext* ctx) override { ctx_ = ctx; return true; }
    bool start(ServerContext* ctx) override {
        if (!ctx->cfg.tcp_conn.enabled.load()) {
            LOG_INFO(LogModule::TCP_LOSS, "TCP conn monitor disabled by config");
            return true;
        }
        monitor_ = std::make_unique<TcpConnMonitor>();
        if (!monitor_->init(ctx->cfg.tcp_conn.bpf_obj.get().c_str(),
                              weaknet::resolveScopePlan(weaknet::MapSizingScope::TcpConn,
                                 {ctx->cfg.tcp_conn.map_sizing.mode.get(), ctx->cfg.tcp_conn.map_sizing.entries.load(),
                                  ctx->cfg.tcp_conn.map_sizing.ram_budget_bp.load()}, weaknet::totalPhysicalRamBytes()))) {
            monitor_.reset();
            return false;
        }
        {
            std::unique_lock<std::shared_mutex> lock(ctx->monitor_pointers_mutex);
            ctx->tcp_conn_monitor = monitor_.get();
        }
        ctx->tcp_conn_stop.store(false);
        start_tcp_conn_monitor_thread(ctx, &worker_, monitor_.get());
        return true;
    }
    void stop() override {
        if (!ctx_) return;
        ctx_->tcp_conn_stop.store(true);
        if (worker_.joinable()) worker_.join();
        {
            std::unique_lock<std::shared_mutex> lock(ctx_->monitor_pointers_mutex);
            if (ctx_->tcp_conn_monitor == monitor_.get()) ctx_->tcp_conn_monitor = nullptr;
            monitor_.reset();
        }
    }
};

// ---------------------------------------------------------------------------
// SkbDropPlugin (skb_drop)
// ---------------------------------------------------------------------------
class SkbDropPlugin : public IMonitorPlugin {
    ServerContext* ctx_ = nullptr;
    std::unique_ptr<SkbDropMonitor> monitor_;
public:
    const char* name() const override { return "skb_drop"; }
    int order() const override { return 160; }
    bool init(ServerContext* ctx) override {
        ctx_ = ctx;
        monitor_ = std::make_unique<SkbDropMonitor>();
        return true;
    }
    bool start(ServerContext* ctx) override {
        if (!monitor_) return false;
        if (ctx && !ctx->cfg.skb_drop.enabled.load()) {
            LOG_INFO(LogModule::NETWORK, "SkbDropPlugin: disabled by configuration");
            return true;
        }
        std::string path = "build/skb_drop.bpf.o";
        weaknet::MapSizingConfig sizing_cfg{"auto", 0, 50};
        if (ctx) {
            path = ctx->cfg.skb_drop.bpf_obj.get();
            sizing_cfg = {ctx->cfg.skb_drop.map_sizing.mode.get(),
                          ctx->cfg.skb_drop.map_sizing.entries.load(),
                          ctx->cfg.skb_drop.map_sizing.ram_budget_bp.load()};
        }
        if (!monitor_->init(path,
                            weaknet::resolveScopePlan(weaknet::MapSizingScope::SkbDrop,
                                                      sizing_cfg, weaknet::totalPhysicalRamBytes()))) {
            LOG_WARNING(LogModule::NETWORK, "SkbDropPlugin: failed to load BPF object from " << path);
            return false;
        }
        if (ctx) {
            std::unique_lock<std::shared_mutex> lock(ctx->monitor_pointers_mutex);
            ctx->skb_drop_monitor = monitor_.get();
        }
        return true;
    }
    void stop() override {
        if (ctx_) {
            std::unique_lock<std::shared_mutex> lock(ctx_->monitor_pointers_mutex);
            ctx_->skb_drop_monitor = nullptr;
        }
        if (monitor_) {
            monitor_->stop();
            monitor_.reset();
        }
    }
};

// ---------------------------------------------------------------------------
// eBPF 插件注册入口
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// TCP 建连可观测性（Stage 2 新增，order 10）
// ---------------------------------------------------------------------------
class TcpConnectPlugin : public IMonitorPlugin {
    ServerContext* ctx_ = nullptr;
    std::thread worker_;
    std::unique_ptr<TcpConnectMonitor> monitor_;
public:
    const char* name() const override { return "tcp_connect"; }
    int order() const override { return 10; }
    bool init(ServerContext* ctx) override { ctx_ = ctx; return true; }
    bool start(ServerContext* ctx) override {
        if (!ctx->cfg.tcp_connect.enabled.load()) {
            LOG_INFO(LogModule::NETWORK, "TCP connect monitor disabled by config");
            return true;
        }
        monitor_ = std::make_unique<TcpConnectMonitor>();
        if (!monitor_->init(ctx->cfg.tcp_connect.bpf_obj.get().c_str(),
                            ctx->cfg.tcp_connect.capture_pages.load())) {
            LOG_WARNING(LogModule::NETWORK, "TcpConnectPlugin: init failed for "
                        << ctx->cfg.tcp_connect.bpf_obj.get());
            monitor_.reset();
            return false;
        }
        {
            std::unique_lock<std::shared_mutex> lock(ctx->monitor_pointers_mutex);
            ctx->tcp_connect_monitor = monitor_.get();
        }
        ctx->tcp_connect_stop.store(false);
        start_tcp_connect_monitor_thread(ctx, &worker_, monitor_.get());
        return true;
    }
    void stop() override {
        if (!ctx_) return;
        ctx_->tcp_connect_stop.store(true);
        if (worker_.joinable()) worker_.join();
        {
            std::unique_lock<std::shared_mutex> lock(ctx_->monitor_pointers_mutex);
            if (ctx_->tcp_connect_monitor == monitor_.get()) ctx_->tcp_connect_monitor = nullptr;
            monitor_.reset();
        }
    }
};

// ---------------------------------------------------------------------------
// 被动 TCP RTT 监控（order 20）
// ---------------------------------------------------------------------------
class TcpRttPlugin : public IMonitorPlugin {
    ServerContext* ctx_ = nullptr;
    std::thread worker_;
    std::unique_ptr<TcpRttMonitor> monitor_;
public:
    const char* name() const override { return "tcp_rtt"; }
    int order() const override { return 20; }
    bool init(ServerContext* ctx) override { ctx_ = ctx; return true; }
    bool start(ServerContext* ctx) override {
        if (!ctx->cfg.tcp_rtt.enabled.load()) {
            LOG_INFO(LogModule::TCP_LOSS, "TCP passive RTT monitor disabled by config");
            return true;
        }
        monitor_ = std::make_unique<TcpRttMonitor>();
        if (!monitor_->init(ctx->cfg.tcp_rtt.bpf_obj.get().c_str(),
                            weaknet::resolveScopePlan(weaknet::MapSizingScope::TcpRtt,
                                {ctx->cfg.tcp_rtt.map_sizing.mode.get(), ctx->cfg.tcp_rtt.map_sizing.entries.load(),
                                 ctx->cfg.tcp_rtt.map_sizing.ram_budget_bp.load()}, weaknet::totalPhysicalRamBytes()))) {
            LOG_WARNING(LogModule::TCP_LOSS, "TcpRttPlugin: init failed for "
                        << ctx->cfg.tcp_rtt.bpf_obj.get());
            monitor_.reset();
            return false;
        }
        {
            std::unique_lock<std::shared_mutex> lock(ctx->monitor_pointers_mutex);
            ctx->tcp_rtt_monitor = monitor_.get();
        }
        ctx->tcp_rtt_stop.store(false);
        start_tcp_rtt_monitor_thread(ctx, &worker_, monitor_.get());
        return true;
    }
    void stop() override {
        if (!ctx_) return;
        ctx_->tcp_rtt_stop.store(true);
        if (worker_.joinable()) worker_.join();
        {
            std::unique_lock<std::shared_mutex> lock(ctx_->monitor_pointers_mutex);
            if (ctx_->tcp_rtt_monitor == monitor_.get()) ctx_->tcp_rtt_monitor = nullptr;
            monitor_.reset();
        }
    }
};

// ---------------------------------------------------------------------------
// 蓝牙设备事件采集（order 10，依赖 bluetooth 插件先启动以拿到 adapter 上下文，
// 但本身不直接依赖它——eBPF 挂内核符号，与 BlueZ D-Bus 状态互相独立）
//
// 所有权：本插件同时拥有 WirelessEventStore 与 BtEventMonitor。
// ServerContext 只保留 store 的裸指针供查询；stop() 时一并清理。
//
// 与 BluetoothPlugin（monitor_plugins.cpp）的职责切分：
//   BluetoothPlugin  = BlueZ D-Bus 状态采集（设备列表/RSSI/连接态）
//   BtEventsPlugin   = 内核 eBPF 事件采集（断连原因 + 归一化事件）
// 两条链路共享 BT 数据域但互不调用，避免一边故障拖垮另一边。
// ---------------------------------------------------------------------------
class BtEventsPlugin : public IMonitorPlugin {
    ServerContext* ctx_ = nullptr;
    std::unique_ptr<WirelessEventStore> store_;
    std::unique_ptr<BtLinkQualityTracker> tracker_;
    std::unique_ptr<SiteIncidentCorrelator> correlator_;
    std::unique_ptr<BtEventMonitor> monitor_;
public:
    const char* name() const override { return "bt_events"; }
    int order() const override { return 10; }
    bool init(ServerContext* ctx) override { ctx_ = ctx; return true; }
    bool start(ServerContext* ctx) override {
        if (!ctx->cfg.bluetooth.enabled.load()) {
            LOG_INFO(LogModule::BLUETOOTH, "bt_events plugin disabled by config (bluetooth.enabled=false)");
            return true;
        }
        if (!ctx->db_mgr) {
            LOG_WARNING(LogModule::BLUETOOTH,
                        "BtEventsPlugin: db_mgr not ready — event store unavailable");
            return true;
        }

        // 事件来源身份：gateway_id 对齐 edge.device_id（云端资产主键），
        // 为空时回落到 hostname，保证每条事件都能回答"是哪台探针看到的"。
        //
        // site_id 来自 edge.site_id 配置：多网关部署时同一现场的多台网关共享
        // 此值，云端据此聚合现场视图。留空则回落 gateway_id —— 即
        // "1 Gateway = 1 Site" 的历史行为完全不变。
        WirelessEventStoreConfig store_cfg;
        store_cfg.gateway_id = ctx->cfg.edge.device_id.get();
        if (store_cfg.gateway_id.empty()) {
            char hostname[256] = {0};
            if (gethostname(hostname, sizeof(hostname) - 1) == 0) {
                store_cfg.gateway_id = hostname;
            }
        }
        const std::string configured_site = ctx->cfg.edge.site_id.get();
        store_cfg.site_id = configured_site.empty() ? store_cfg.gateway_id : configured_site;

        store_ = std::make_unique<WirelessEventStore>(ctx->db_mgr.get(), std::move(store_cfg));
        ctx->wireless_event_store = store_.get();

        // Phase 2: 初始化链路质量跟踪器，并预热加载历史基线画像
        tracker_ = std::make_unique<BtLinkQualityTracker>();
        ctx->bt_link_quality_tracker = tracker_.get();

        // Phase 3a: 区域级异常关联器——把散落的设备事件聚合成 SiteIncident。
        //
        // 构造顺序必须是 correlator → store（store 只存裸指针，无依赖），
        // 但**回放必须早于绑定**：
        //   1. 先 recoverFromStore() 恢复内存态。此时 store 尚未绑定关联器，
        //      回放产出的事件不会反过来被重新投递进关联器（否则每条回放事件
        //      都会触发一次关联，造成重复计数与无谓的 DB 写）。
        //   2. 再绑定。此后运行期事件才自动进入关联。
        //
        // 关联器的 site_id 与 store 一致（Phase 3a 运行模型 1 Gateway = 1 Site），
        // 五个阈值从配置读入——这些键是关联器的**构造期**配置，运行时改完需
        // `weaknet-cli monitor restart bt_events` 重建本插件才生效。
        SiteIncidentConfig incident_cfg;
        incident_cfg.site_id = store_->config().site_id;
        incident_cfg.min_affected_devices =
            ctx->cfg.bluetooth.incident_min_devices.load();
        incident_cfg.min_affected_ratio =
            ctx->cfg.bluetooth.incident_min_ratio_bp.load() / 10000.0;  // bp → 比例
        incident_cfg.correlation_window_ms =
            ctx->cfg.bluetooth.incident_window_ms.load();
        incident_cfg.quiet_window_ms =
            ctx->cfg.bluetooth.incident_quiet_window_ms.load();
        incident_cfg.active_device_memory_ms =
            ctx->cfg.bluetooth.incident_active_window_ms.load();
        correlator_ = std::make_unique<SiteIncidentCorrelator>(ctx->db_mgr.get(), incident_cfg);
        ctx->site_incident_correlator = correlator_.get();

        // 回放窗口右沿用当前墙钟：关联器只按事件时间做决策，
        // 这个 now_ms 只用于确定"最近多久的事件值得回放"。
        const uint64_t now_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        correlator_->recoverFromStore(now_ms);
        store_->setIncidentCorrelator(correlator_.get());

        monitor_ = std::make_unique<BtEventMonitor>(store_.get(), tracker_.get());
        if (!monitor_->init(ctx->cfg.bluetooth.events_bpf_obj.get(),
                            store_->config().gateway_id)) {
            LOG_WARNING(LogModule::BLUETOOTH, "BtEventsPlugin: init failed for "
                        << ctx->cfg.bluetooth.events_bpf_obj.get()
                        << " — bluetooth disconnect reason capture disabled ("
                        << monitor_->lastError() << ")");
            // 不是致命错误：事件采集失败不影响其它监控器，
            // 但 store 和 tracker 仍然有效（D-Bus 路径仍可产生 LinkDegraded）
            monitor_.reset();
            return true;
        }
        if (!monitor_->start()) {
            LOG_WARNING(LogModule::BLUETOOTH, "BtEventsPlugin: start failed: "
                        << monitor_->lastError());
            monitor_.reset();
            return true;
        }

        LOG_INFO(LogModule::BLUETOOTH, "BtEventMonitor running, attached hooks: "
                 << monitor_->attachedHooksSummary());
        return true;
    }
    void stop() override {
        if (monitor_) {
            monitor_->stop();
            monitor_.reset();
        }
        // 先解绑再销毁：store 在关联器之前构造、之后销毁，
        // 中间任何时刻 recordEvent 都不能看到一个悬垂的关联器指针。
        if (store_) {
            store_->setIncidentCorrelator(nullptr);
        }
        if (ctx_) {
            if (ctx_->wireless_event_store == store_.get()) {
                ctx_->wireless_event_store = nullptr;
            }
            if (ctx_->bt_link_quality_tracker == tracker_.get()) {
                ctx_->bt_link_quality_tracker = nullptr;
            }
            if (ctx_->site_incident_correlator == correlator_.get()) {
                ctx_->site_incident_correlator = nullptr;
            }
        }
        correlator_.reset();
        tracker_.reset();
        store_.reset();
    }
};

// ---------------------------------------------------------------------------
// 工业蓝牙 HCI ACL 数据通道流量监测（Phase 1 新架构，order 10）
// ---------------------------------------------------------------------------
class BtHciTrafficPlugin : public IMonitorPlugin {
    ServerContext* ctx_ = nullptr;
    std::shared_ptr<weaknet::BtHciConnectionTracker> tracker_;
    std::unique_ptr<BtDataPathAnalyzer> analyzer_;
    std::unique_ptr<BtDataPathAnomalyDetector> anomaly_detector_;
    std::thread worker_;
    std::atomic<bool> stop_{false};
public:
    const char* name() const override { return "bt_hci_traffic"; }
    int order() const override { return 10; }
    bool init(ServerContext* ctx) override { ctx_ = ctx; return true; }
    bool start(ServerContext* ctx) override {
        if (!ctx->cfg.bluetooth.enabled.load()) {
            LOG_INFO(LogModule::BLUETOOTH, "bt_hci_traffic plugin disabled by config (bluetooth.enabled=false)");
            return true;
        }

        tracker_ = std::make_shared<weaknet::BtHciConnectionTracker>();
        analyzer_ = std::make_unique<BtDataPathAnalyzer>(tracker_);

        BtAnomalyDetectorConfig anomaly_cfg;
        anomaly_cfg.stall_timeout_ms = ctx->cfg.bluetooth.stall_timeout_ms.load();
        anomaly_cfg.stall_consecutive_windows = ctx->cfg.bluetooth.stall_consecutive_windows.load();
        anomaly_cfg.min_expected_bps = static_cast<double>(ctx->cfg.bluetooth.min_expected_bps.load());
        anomaly_cfg.degraded_consecutive_windows = ctx->cfg.bluetooth.degraded_consecutive_windows.load();
        anomaly_detector_ = std::make_unique<BtDataPathAnomalyDetector>(anomaly_cfg);

        std::string bpf_path = ctx->cfg.bluetooth.hci_traffic_bpf_obj.get();
        if (!analyzer_->init(bpf_path, "hci0")) {
            LOG_WARNING(LogModule::BLUETOOTH, "BtHciTrafficPlugin: init failed for "
                        << bpf_path << " — falling back to offline mode ("
                        << analyzer_->lastError() << ")");
        }

        {
            std::unique_lock<std::shared_mutex> lock(ctx->monitor_pointers_mutex);
            ctx->bt_hci_analyzer = analyzer_.get();
            ctx->bt_anomaly_detector = anomaly_detector_.get();
        }

        stop_.store(false);
        worker_ = std::thread([this]() {
            int sync_countdown = 0;
            while (!stop_.load() && ctx_ && ctx_->running.load()) {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                if (stop_.load()) break;

                // 周期性同步内核连接表（每 4 秒同步一次，即 2 个 tick）
                if (tracker_ && (++sync_countdown % 2 == 0)) {
                    tracker_->syncFromKernel("hci0");
                }

                if (analyzer_ && analyzer_->isAvailable()) {
                    auto obs = analyzer_->collectObservations();
                    if (!obs.empty()) {
                        LOG_DEBUG(LogModule::BLUETOOTH, "BtDataPathAnalyzer collected "
                                  << obs.size() << " observations");
                        const int64_t now_ms = static_cast<int64_t>(
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch()).count());

                        if (ctx_ && ctx_->db_mgr) {
                            for (const auto& item : obs) {
                                // 仅持久化有活跃流量或已知设备的观测，避免无用全零刷库
                                if (item.tx_packets > 0 || item.rx_packets > 0 || item.device_id.has_value()) {
                                    ctx_->db_mgr->insertBtDataPathMetric(
                                        now_ms,
                                        item.controller_id,
                                        item.connection_handle,
                                        item.connection_generation,
                                        item.attribution_scope,
                                        item.device_id.value_or(""),
                                        item.window_ms,
                                        item.tx_bytes,
                                        item.rx_bytes,
                                        item.tx_packets,
                                        item.rx_packets,
                                        item.tx_throughput_bps,
                                        item.rx_throughput_bps,
                                        item.max_gap_ms
                                    );
                                }
                            }
                        }

                        // Phase 2：运行规则驱动 L2 异常评估
                        if (anomaly_detector_) {
                            // 动态吸收最新运行时配置
                            BtAnomalyDetectorConfig cur_cfg;
                            cur_cfg.stall_timeout_ms = ctx_->cfg.bluetooth.stall_timeout_ms.load();
                            cur_cfg.stall_consecutive_windows = ctx_->cfg.bluetooth.stall_consecutive_windows.load();
                            cur_cfg.min_expected_bps = static_cast<double>(ctx_->cfg.bluetooth.min_expected_bps.load());
                            cur_cfg.degraded_consecutive_windows = ctx_->cfg.bluetooth.degraded_consecutive_windows.load();
                            anomaly_detector_->updateConfig(cur_cfg);

                            auto anomalies = anomaly_detector_->evaluate(obs, now_ms);
                            for (const auto& anomaly : anomalies) {
                                LOG_WARNING(LogModule::BLUETOOTH, "L2 BT Anomaly detected: ["
                                            << anomaly.anomaly_type << "] handle=" << anomaly.connection_handle
                                            << " device=" << anomaly.device_address << ": "
                                            << anomaly.description);

                                // 若已绑定具体设备，桥接生成一条规范化设备事件 (LinkDegraded) 注入事件存储
                                if (!anomaly.device_address.empty() && ctx_->wireless_event_store) {
                                    WirelessDeviceEvent ev;
                                    ev.device_address = anomaly.device_address;
                                    ev.event_type = DeviceEventType::LinkDegraded;
                                    ev.protocol = WirelessProtocol::Bluetooth;
                                    ev.source = EvidenceSource::Derived;
                                    ev.source_detail = anomaly.anomaly_type;
                                    ev.suspected_cause = anomaly.description;
                                    ev.timestamp_ms = static_cast<uint64_t>(now_ms);
                                    ev.details_json = anomaly.toJson();
                                    ctx_->wireless_event_store->recordEvent(ev);
                                }
                            }
                        }
                    }
                }
            }
        });

        LOG_INFO(LogModule::BLUETOOTH, "BtHciTrafficPlugin started successfully, hooks: "
                 << analyzer_->attachedHookName());
        return true;
    }
    void stop() override {
        stop_.store(true);
        if (worker_.joinable()) {
            worker_.join();
        }
        if (analyzer_) {
            analyzer_->stop();
        }
        if (ctx_) {
            std::unique_lock<std::shared_mutex> lock(ctx_->monitor_pointers_mutex);
            if (ctx_->bt_hci_analyzer == analyzer_.get()) {
                ctx_->bt_hci_analyzer = nullptr;
            }
            if (ctx_->bt_anomaly_detector == anomaly_detector_.get()) {
                ctx_->bt_anomaly_detector = nullptr;
            }
        }
        analyzer_.reset();
        anomaly_detector_.reset();
        tracker_.reset();
    }
};

void registerEbpfPlugins() {
    registerPlugin("dns",             [] { return std::make_unique<DnsPlugin>(); });
    registerPlugin("wifi_loss",       [] { return std::make_unique<WifiLossPlugin>(); });
    registerPlugin("http_latency",    [] { return std::make_unique<HttpLatencyPlugin>(); });
    registerPlugin("process_profiler",[] { return std::make_unique<ProcessProfilerPlugin>(); });
    registerPlugin("tcp_retrans",     [] { return std::make_unique<TcpRetransPlugin>(); });
    registerPlugin("tcp_conn",        [] { return std::make_unique<TcpConnPlugin>(); });
    registerPlugin("skb_drop",        [] { return std::make_unique<SkbDropPlugin>(); });
    registerPlugin("tcp_connect",     [] { return std::make_unique<TcpConnectPlugin>(); });
    registerPlugin("tcp_rtt",         [] { return std::make_unique<TcpRttPlugin>(); });
    registerPlugin("bt_events",       [] { return std::make_unique<BtEventsPlugin>(); });
    registerPlugin("bt_hci_traffic",  [] { return std::make_unique<BtHciTrafficPlugin>(); });
}

}  // namespace weaknet_dbus