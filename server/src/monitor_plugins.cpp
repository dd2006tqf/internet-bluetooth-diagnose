/**
 * @file monitor_plugins.cpp
 * @brief 内置监控器插件实现（传统监控器组）
 *
 * 将 9 个传统监控线程包装为 IMonitorPlugin，注册进静态注册表。
 * 生命周期三阶段：
 *   - init:   记录 ctx（传统监控器无额外资源）
 *   - start:  执行 enabled 守卫后调用对应 start_xxx_thread
 *   - stop:   Phase A 暂空（线程 join 仍由 server.cpp 统一做，保证退出顺序零变化）
 *
 * 注册方式：server.cpp 在启动前调用 registerBuiltinPlugins()。
 * 不使用静态初始化自注册（避免静态库链接时对象被丢弃导致注册缺失）。
 */

#include "monitor_registry.hpp"
#include "logger.hpp"

#include "server.hpp"
#include "bt_monitor.hpp"
#include "rtt_monitor.hpp"
#include "jitter_monitor.hpp"
#include "rssi_monitor.hpp"
#include "tcp_loss_monitor.hpp"

namespace weaknet_dbus {

// ---------------------------------------------------------------------------
// 网卡列表监控（order 0：基础数据源）
// ---------------------------------------------------------------------------
class IfacePlugin : public IMonitorPlugin {
    ServerContext* ctx_ = nullptr;
public:
    const char* name() const override { return "iface"; }
    int order() const override { return 0; }
    bool init(ServerContext* ctx) override { ctx_ = ctx; return true; }
    bool start(ServerContext* ctx) override {
        start_iface_monitor_thread(ctx);
        return true;
    }
    void stop() override {}
};

// ---------------------------------------------------------------------------
// 当前上网网卡监控（order 0）
// ---------------------------------------------------------------------------
class UsingIfacePlugin : public IMonitorPlugin {
public:
    const char* name() const override { return "using_iface"; }
    int order() const override { return 0; }
    bool init(ServerContext* ctx) override { (void)ctx; return true; }
    bool start(ServerContext* ctx) override {
        start_using_iface_thread(ctx);
        return true;
    }
    void stop() override {}
};

// ---------------------------------------------------------------------------
// RTT 延迟监控（order 10）
// ---------------------------------------------------------------------------
class RttPlugin : public IMonitorPlugin {
public:
    const char* name() const override { return "rtt"; }
    int order() const override { return 10; }
    bool init(ServerContext* ctx) override { (void)ctx; return true; }
    bool start(ServerContext* ctx) override {
        if (!ctx->cfg.rtt.enabled.load()) {
            LOG_INFO(LogModule::RTT, "RTT monitor disabled by config");
            return true;
        }
        start_rtt_monitor_thread(ctx,
            ctx->cfg.rtt.target.get(),
            ctx->cfg.rtt.interval_ms.load(),
            ctx->cfg.rtt.timeout_ms.load());
        return true;
    }
    void stop() override {}
};

// ---------------------------------------------------------------------------
// Jitter 抖动监控（order 10）
// ---------------------------------------------------------------------------
class JitterPlugin : public IMonitorPlugin {
public:
    const char* name() const override { return "jitter"; }
    int order() const override { return 10; }
    bool init(ServerContext* ctx) override { (void)ctx; return true; }
    bool start(ServerContext* ctx) override {
        if (!ctx->cfg.jitter.enabled.load()) {
            LOG_INFO(LogModule::NETWORK, "Jitter monitor disabled by config");
            return true;
        }
        start_jitter_monitor_thread(ctx,
            ctx->cfg.jitter.target.get(),
            ctx->cfg.jitter.interval_ms.load(),
            ctx->cfg.jitter.timeout_ms.load(),
            ctx->cfg.jitter.window_size.load());
        return true;
    }
    void stop() override {}
};

// ---------------------------------------------------------------------------
// Wi-Fi RSSI 监控（order 10）
// ---------------------------------------------------------------------------
class RssiPlugin : public IMonitorPlugin {
public:
    const char* name() const override { return "rssi"; }
    int order() const override { return 10; }
    bool init(ServerContext* ctx) override { (void)ctx; return true; }
    bool start(ServerContext* ctx) override {
        if (!ctx->cfg.rssi.enabled.load()) {
            LOG_INFO(LogModule::RSSI, "RSSI monitor disabled by config");
            return true;
        }
        start_rssi_monitor_thread(ctx, "");  // ctrlDir 留空 → wpa_supplicant 默认路径
        return true;
    }
    void stop() override {}
};

// ---------------------------------------------------------------------------
// TCP 丢包率监控（order 10）
// ---------------------------------------------------------------------------
class TcpLossPlugin : public IMonitorPlugin {
public:
    const char* name() const override { return "tcp_loss"; }
    int order() const override { return 10; }
    bool init(ServerContext* ctx) override { (void)ctx; return true; }
    bool start(ServerContext* ctx) override {
        if (!ctx->cfg.tcp_loss.enabled.load()) {
            LOG_INFO(LogModule::TCP_LOSS, "TCP loss monitor disabled by config");
            return true;
        }
        start_tcp_loss_monitor_thread(ctx);
        return true;
    }
    void stop() override {}
};

// ---------------------------------------------------------------------------
// 流量分析（order 10：flow_rate.bpf.o 的持有者，先于其他 eBPF 消费插件）
// ---------------------------------------------------------------------------
class TrafficPlugin : public IMonitorPlugin {
public:
    const char* name() const override { return "traffic"; }
    int order() const override { return 10; }
    bool init(ServerContext* ctx) override { (void)ctx; return true; }
    bool start(ServerContext* ctx) override {
        if (!ctx->cfg.traffic.enabled.load()) {
            LOG_INFO(LogModule::WEAK_MGR, "Traffic analysis disabled by config");
            return true;
        }
        start_traffic_analysis_thread(ctx);
        return true;
    }
    void stop() override {}
};

// ---------------------------------------------------------------------------
// 网络质量综合评估（order 10）
// ---------------------------------------------------------------------------
class QualityPlugin : public IMonitorPlugin {
public:
    const char* name() const override { return "quality"; }
    int order() const override { return 10; }
    bool init(ServerContext* ctx) override { (void)ctx; return true; }
    bool start(ServerContext* ctx) override {
        if (!ctx->cfg.quality.enabled.load()) {
            LOG_INFO(LogModule::WEAK_MGR, "Network quality monitor disabled by config");
            return true;
        }
        start_network_quality_thread(ctx);
        return true;
    }
    void stop() override {}
};

// ---------------------------------------------------------------------------
// 蓝牙监测（order 10）
// ---------------------------------------------------------------------------
class BluetoothPlugin : public IMonitorPlugin {
public:
    const char* name() const override { return "bluetooth"; }
    int order() const override { return 10; }
    bool init(ServerContext* ctx) override { (void)ctx; return true; }
    bool start(ServerContext* ctx) override {
        if (!ctx->cfg.bluetooth.enabled.load()) {
            LOG_INFO(LogModule::BLUETOOTH, "Bluetooth monitor disabled by config");
            return true;
        }
        // 蓝牙监测器实例由插件创建（原 server.cpp 显式创建）
        ctx->bt_monitor = std::make_unique<BtMonitor>();
        start_bt_monitor_thread(ctx, nullptr);
        return true;
    }
    void stop() override {}
};

// ---------------------------------------------------------------------------
// 内置插件注册入口（server.cpp 启动前调用）
// ---------------------------------------------------------------------------
void registerBuiltinPlugins() {
    registerPlugin("iface",        [] { return std::make_unique<IfacePlugin>(); });
    registerPlugin("using_iface",  [] { return std::make_unique<UsingIfacePlugin>(); });
    registerPlugin("rtt",          [] { return std::make_unique<RttPlugin>(); });
    registerPlugin("jitter",       [] { return std::make_unique<JitterPlugin>(); });
    registerPlugin("rssi",         [] { return std::make_unique<RssiPlugin>(); });
    registerPlugin("tcp_loss",     [] { return std::make_unique<TcpLossPlugin>(); });
    registerPlugin("traffic",      [] { return std::make_unique<TrafficPlugin>(); });
    registerPlugin("quality",      [] { return std::make_unique<QualityPlugin>(); });
    registerPlugin("bluetooth",    [] { return std::make_unique<BluetoothPlugin>(); });
}

}  // namespace weaknet_dbus