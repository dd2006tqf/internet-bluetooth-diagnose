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
#include "bt_event_monitor.hpp"
#include "wireless_event_store.hpp"
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
        ctx->dns_monitor = monitor_.get();
        ctx->dns_stop.store(false);
        start_dns_monitor_thread(ctx, &worker_, monitor_.get());
        return true;
    }
    void stop() override {
        if (!ctx_) return;
        ctx_->dns_stop.store(true);
        if (worker_.joinable()) worker_.join();
        if (ctx_->dns_monitor == monitor_.get()) ctx_->dns_monitor = nullptr;
        monitor_.reset();
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
        ctx->wifi_loss_monitor = monitor_.get();
        ctx->wifi_loss_stop.store(false);
        start_wifi_loss_monitor_thread(ctx, &worker_, monitor_.get());
        return true;
    }
    void stop() override {
        if (!ctx_) return;
        ctx_->wifi_loss_stop.store(true);
        if (worker_.joinable()) worker_.join();
        if (ctx_->wifi_loss_monitor == monitor_.get()) ctx_->wifi_loss_monitor = nullptr;
        monitor_.reset();
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
        ctx->http_latency_monitor = monitor_.get();
        ctx->http_latency_stop.store(false);
        start_http_latency_monitor_thread(ctx, &worker_, monitor_.get());
        return true;
    }
    void stop() override {
        if (!ctx_) return;
        ctx_->http_latency_stop.store(true);
        if (worker_.joinable()) worker_.join();
        if (ctx_->http_latency_monitor == monitor_.get()) ctx_->http_latency_monitor = nullptr;
        monitor_.reset();
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
        ctx->process_net_profiler = monitor_.get();
        ctx->process_profiler_stop.store(false);
        start_process_net_profiler_thread(ctx, &worker_, monitor_.get());
        return true;
    }
    void stop() override {
        if (!ctx_) return;
        ctx_->process_profiler_stop.store(true);
        if (worker_.joinable()) worker_.join();
        if (ctx_->process_net_profiler == monitor_.get()) ctx_->process_net_profiler = nullptr;
        monitor_.reset();
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
        ctx->tcp_retrans_monitor = monitor_.get();
        ctx->tcp_retrans_stop.store(false);
        start_tcp_retrans_monitor_thread(ctx, &worker_, monitor_.get());
        return true;
    }
    void stop() override {
        if (!ctx_) return;
        ctx_->tcp_retrans_stop.store(true);
        if (worker_.joinable()) worker_.join();
        if (ctx_->tcp_retrans_monitor == monitor_.get()) ctx_->tcp_retrans_monitor = nullptr;
        monitor_.reset();
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
        ctx->tcp_conn_monitor = monitor_.get();
        ctx->tcp_conn_stop.store(false);
        start_tcp_conn_monitor_thread(ctx, &worker_, monitor_.get());
        return true;
    }
    void stop() override {
        if (!ctx_) return;
        ctx_->tcp_conn_stop.store(true);
        if (worker_.joinable()) worker_.join();
        if (ctx_->tcp_conn_monitor == monitor_.get()) ctx_->tcp_conn_monitor = nullptr;
        monitor_.reset();
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
            ctx->skb_drop_monitor = monitor_.get();
        }
        return true;
    }
    void stop() override {
        if (ctx_) {
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
        ctx->tcp_connect_monitor = monitor_.get();
        ctx->tcp_connect_stop.store(false);
        start_tcp_connect_monitor_thread(ctx, &worker_, monitor_.get());
        return true;
    }
    void stop() override {
        if (!ctx_) return;
        ctx_->tcp_connect_stop.store(true);
        if (worker_.joinable()) worker_.join();
        if (ctx_->tcp_connect_monitor == monitor_.get()) ctx_->tcp_connect_monitor = nullptr;
        monitor_.reset();
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
        // Phase 1 运行模型是 1 Gateway = 1 Site，因此 site 直接取 gateway。
        WirelessEventStoreConfig store_cfg;
        store_cfg.gateway_id = ctx->cfg.edge.device_id.get();
        if (store_cfg.gateway_id.empty()) {
            char hostname[256] = {0};
            if (gethostname(hostname, sizeof(hostname) - 1) == 0) {
                store_cfg.gateway_id = hostname;
            }
        }
        store_cfg.site_id = store_cfg.gateway_id;

        store_ = std::make_unique<WirelessEventStore>(ctx->db_mgr.get(), std::move(store_cfg));
        ctx->wireless_event_store = store_.get();

        monitor_ = std::make_unique<BtEventMonitor>(store_.get());
        if (!monitor_->init(ctx->cfg.bluetooth.events_bpf_obj.get(),
                            store_->config().gateway_id)) {
            LOG_WARNING(LogModule::BLUETOOTH, "BtEventsPlugin: init failed for "
                        << ctx->cfg.bluetooth.events_bpf_obj.get()
                        << " — bluetooth disconnect reason capture disabled ("
                        << monitor_->lastError() << ")");
            // 不是致命错误：事件采集失败不影响其它监控器，
            // 但 store 仍然有效（后续 Phase 2/3 的事件可从 D-Bus 路径进入）
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
        if (ctx_) {
            if (ctx_->wireless_event_store == store_.get()) {
                ctx_->wireless_event_store = nullptr;
            }
        }
        store_.reset();
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
    registerPlugin("bt_events",       [] { return std::make_unique<BtEventsPlugin>(); });
}

}  // namespace weaknet_dbus