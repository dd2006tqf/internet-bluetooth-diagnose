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
#include "logger.hpp"

#include "server.hpp"
#include "dns_monitor.hpp"
#include "wifi_packet_loss_monitor.hpp"
#include "http_latency_monitor.hpp"
#include "process_net_profiler.hpp"
#include "tcp_retransmit_monitor.hpp"
#include "tcp_conn_monitor.hpp"
#include "skb_drop_monitor.hpp"

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
        if (!monitor_->init(ctx->cfg.dns.bpf_obj.get().c_str())) {
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
        if (!monitor_->init(ctx->cfg.http_latency.bpf_obj.get().c_str())) {
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
        if (!monitor_->init(ctx->cfg.process_profiler.bpf_obj.get().c_str())) {
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
        if (!monitor_->init(ctx->cfg.tcp_retrans.bpf_obj.get().c_str())) {
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
        if (!monitor_->init(ctx->cfg.tcp_conn.bpf_obj.get().c_str())) {
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
        std::string path = "build/skb_drop.bpf.o";
        if (ctx) {
            path = ctx->cfg.skb_drop.bpf_obj.get();
        }
        if (!monitor_->init(path)) {
            LOG_WARNING(LogModule::NETWORK, "SkbDropPlugin: failed to load BPF object from " << path);
            return false;
        }
        return true;
    }
    void stop() override {
        if (monitor_) {
            monitor_->stop();
            monitor_.reset();
        }
    }
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
    registerPlugin("skb_drop",        [] { return std::make_unique<SkbDropPlugin>(); });
}

}  // namespace weaknet_dbus