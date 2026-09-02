/**
 * @file monitor_ebpf_plugins.cpp
 * @brief 内置 eBPF 监控器插件实现
 *
 * 6 个 eBPF 监控器插件，复用现有 IEbpfMonitor 实现。
 * order 设计：
 *   10: dns / wifi_loss / http_latency（无共享依赖）
 *   10: traffic（flow_rate 持有者，已在传统组注册）
 *   20: process_profiler（依赖 traffic 的 flow_rate）
 *   20: tcp_retrans / tcp_conn（独立 eBPF 对象）
 */

#include "monitor_registry.hpp"
#include "logger.hpp"

#include "server.hpp"
#include "dns_monitor.hpp"
#include "wifi_packet_loss_monitor.hpp"
#include "http_latency_monitor.hpp"
#include "process_net_profiler.hpp"
#include "tcp_retransmit_monitor.hpp"
#include "tcp_conn_monitor.hpp"

namespace weaknet_dbus {

// ---------------------------------------------------------------------------
// DNS 监控（order 10）
// ---------------------------------------------------------------------------
class DnsPlugin : public IMonitorPlugin {
public:
    const char* name() const override { return "dns"; }
    int order() const override { return 10; }
    bool init(ServerContext* ctx) override { (void)ctx; return true; }
    bool start(ServerContext* ctx) override {
        if (!ctx->cfg.dns.enabled.load()) {
            LOG_INFO(LogModule::NETWORK, "DNS monitor disabled by config");
            return true;
        }
        ctx->dns_monitor = std::make_unique<DnsMonitor>();
        start_dns_monitor_thread(ctx);
        return true;
    }
    void stop() override {}
};

// ---------------------------------------------------------------------------
// Wi-Fi 丢包归因（order 10）
// ---------------------------------------------------------------------------
class WifiLossPlugin : public IMonitorPlugin {
public:
    const char* name() const override { return "wifi_loss"; }
    int order() const override { return 10; }
    bool init(ServerContext* ctx) override { (void)ctx; return true; }
    bool start(ServerContext* ctx) override {
        if (!ctx->cfg.wifi_loss.enabled.load()) {
            LOG_INFO(LogModule::NETWORK, "Wi-Fi loss monitor disabled by config");
            return true;
        }
        ctx->wifi_loss_monitor = std::make_unique<WifiPacketLossMonitor>();
        start_wifi_loss_monitor_thread(ctx);
        return true;
    }
    void stop() override {}
};

// ---------------------------------------------------------------------------
// HTTP 延迟监控（order 10）
// ---------------------------------------------------------------------------
class HttpLatencyPlugin : public IMonitorPlugin {
public:
    const char* name() const override { return "http_latency"; }
    int order() const override { return 10; }
    bool init(ServerContext* ctx) override { (void)ctx; return true; }
    bool start(ServerContext* ctx) override {
        if (!ctx->cfg.http_latency.enabled.load()) {
            LOG_INFO(LogModule::NETWORK, "HTTP latency monitor disabled by config");
            return true;
        }
        ctx->http_latency_monitor = std::make_unique<HttpLatencyMonitor>();
        start_http_latency_monitor_thread(ctx);
        return true;
    }
    void stop() override {}
};

// ---------------------------------------------------------------------------
// 进程网络画像（order 20：依赖 traffic 的 flow_rate，需在 traffic 之后启动）
// ---------------------------------------------------------------------------
class ProcessProfilerPlugin : public IMonitorPlugin {
public:
    const char* name() const override { return "process_profiler"; }
    int order() const override { return 20; }
    bool init(ServerContext* ctx) override { (void)ctx; return true; }
    bool start(ServerContext* ctx) override {
        if (!ctx->cfg.process_profiler.enabled.load()) {
            LOG_INFO(LogModule::NETWORK, "Process net profiler disabled by config");
            return true;
        }
        ctx->process_net_profiler = std::make_unique<ProcessNetProfiler>();
        start_process_net_profiler_thread(ctx);
        return true;
    }
    void stop() override {}
};

// ---------------------------------------------------------------------------
// TCP 重传监控（order 20）
// ---------------------------------------------------------------------------
class TcpRetransPlugin : public IMonitorPlugin {
public:
    const char* name() const override { return "tcp_retrans"; }
    int order() const override { return 20; }
    bool init(ServerContext* ctx) override { (void)ctx; return true; }
    bool start(ServerContext* ctx) override {
        if (!ctx->cfg.tcp_retrans.enabled.load()) {
            LOG_INFO(LogModule::NETWORK, "TCP retransmit monitor disabled by config");
            return true;
        }
        ctx->tcp_retrans_monitor = std::make_unique<TcpRetransMonitor>();
        start_tcp_retrans_monitor_thread(ctx);
        return true;
    }
    void stop() override {}
};

// ---------------------------------------------------------------------------
// TCP 连接生命周期（order 20）
// ---------------------------------------------------------------------------
class TcpConnPlugin : public IMonitorPlugin {
public:
    const char* name() const override { return "tcp_conn"; }
    int order() const override { return 20; }
    bool init(ServerContext* ctx) override { (void)ctx; return true; }
    bool start(ServerContext* ctx) override {
        if (!ctx->cfg.tcp_conn.enabled.load()) {
            LOG_INFO(LogModule::TCP_LOSS, "TCP conn monitor disabled by config");
            return true;
        }
        ctx->tcp_conn_monitor = std::make_unique<TcpConnMonitor>();
        start_tcp_conn_monitor_thread(ctx);
        return true;
    }
    void stop() override {}
};

// ---------------------------------------------------------------------------
// eBPF 插件注册入口
// ---------------------------------------------------------------------------
void registerEbpfPlugins() {
    registerPlugin("dns",             [] { return std::make_unique<DnsPlugin>(); });
    registerPlugin("wifi_loss",       [] { return std::make_unique<WifiLossPlugin>(); });
    registerPlugin("http_latency",    [] { return std::make_unique<HttpLatencyPlugin>(); });
    registerPlugin("process_profiler",[] { return std::make_unique<ProcessProfilerPlugin>(); });
    registerPlugin("tcp_retrans",     [] { return std::make_unique<TcpRetransPlugin>(); });
    registerPlugin("tcp_conn",        [] { return std::make_unique<TcpConnPlugin>(); });
}

}  // namespace weaknet_dbus