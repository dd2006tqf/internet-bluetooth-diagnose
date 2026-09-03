/**
 * @file monitor_plugins.cpp
 * @brief 内置监控器插件实现（传统监控器组）
 *
 * 将 9 个传统监控线程包装为 IMonitorPlugin，注册进静态注册表。
 * 生命周期三阶段：
 *   - init:   记录 ctx（传统监控器无额外资源）
 *   - start:  执行 enabled 守卫后调用对应 start_xxx_thread
 *   - stop:   请求对应停止标志并 join 该插件的线程（阶段二过渡；线程句柄暂存于 ServerContext）
 *
 * 注册方式：server.cpp 在启动前调用 registerBuiltinPlugins()。
 * 不使用静态初始化自注册（避免静态库链接时对象被丢弃导致注册缺失）。
 */

#include "monitor_registry.hpp"
#include <thread>
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
    std::thread worker_;
public:
    const char* name() const override { return "iface"; }
    int order() const override { return 0; }
    bool init(ServerContext* ctx) override { ctx_ = ctx; return true; }
    bool start(ServerContext* ctx) override {
        ctx->iface_stop.store(false);
        start_iface_monitor_thread(ctx, &worker_);
        return true;
    }
    void stop() override {
        if (!ctx_) return;
        ctx_->iface_stop.store(true);
        if (worker_.joinable()) worker_.join();
    }
};

// ---------------------------------------------------------------------------
// 当前上网网卡监控（order 0）
// ---------------------------------------------------------------------------
class UsingIfacePlugin : public IMonitorPlugin {
    ServerContext* ctx_ = nullptr;
    std::thread worker_;
public:
    const char* name() const override { return "using_iface"; }
    int order() const override { return 0; }
    bool init(ServerContext* ctx) override { ctx_ = ctx; return true; }
    bool start(ServerContext* ctx) override {
        ctx->using_iface_stop.store(false);
        start_using_iface_thread(ctx, &worker_);
        return true;
    }
    void stop() override {
        if (!ctx_) return;
        ctx_->using_iface_stop.store(true);
        if (worker_.joinable()) worker_.join();
    }
};

// ---------------------------------------------------------------------------
// RTT 延迟监控（order 10）
// ---------------------------------------------------------------------------
class RttPlugin : public IMonitorPlugin {
    ServerContext* ctx_ = nullptr;
    std::thread worker_;
public:
    const char* name() const override { return "rtt"; }
    int order() const override { return 10; }
    bool init(ServerContext* ctx) override { ctx_ = ctx; return true; }
    bool start(ServerContext* ctx) override {
        if (!ctx->cfg.rtt.enabled.load()) {
            LOG_INFO(LogModule::RTT, "RTT monitor disabled by config");
            return true;
        }
        ctx->rtt_stop.store(false);
        start_rtt_monitor_thread(ctx, &worker_,
            ctx->cfg.rtt.target.get(),
            ctx->cfg.rtt.interval_ms.load(),
            ctx->cfg.rtt.timeout_ms.load());
        return true;
    }
    void stop() override {
        if (!ctx_) return;
        ctx_->rtt_stop.store(true);
        if (worker_.joinable()) worker_.join();
    }
};

// ---------------------------------------------------------------------------
// Jitter 抖动监控（order 10）
// ---------------------------------------------------------------------------
class JitterPlugin : public IMonitorPlugin {
    ServerContext* ctx_ = nullptr;
    std::thread worker_;
public:
    const char* name() const override { return "jitter"; }
    int order() const override { return 10; }
    std::vector<std::string> dependencies() const override { return {"rtt"}; }
    bool init(ServerContext* ctx) override { ctx_ = ctx; return true; }
    bool start(ServerContext* ctx) override {
        if (!ctx->cfg.jitter.enabled.load()) {
            LOG_INFO(LogModule::NETWORK, "Jitter monitor disabled by config");
            return true;
        }
        ctx->jitter_stop.store(false);
        start_jitter_monitor_thread(ctx, &worker_,
            ctx->cfg.jitter.target.get(),
            ctx->cfg.jitter.interval_ms.load(),
            ctx->cfg.jitter.timeout_ms.load(),
            ctx->cfg.jitter.window_size.load());
        return true;
    }
    void stop() override {
        if (!ctx_) return;
        ctx_->jitter_stop.store(true);
        if (worker_.joinable()) worker_.join();
    }
};

// ---------------------------------------------------------------------------
// Wi-Fi RSSI 监控（order 10）
// ---------------------------------------------------------------------------
class RssiPlugin : public IMonitorPlugin {
    ServerContext* ctx_ = nullptr;
    std::thread worker_;
public:
    const char* name() const override { return "rssi"; }
    int order() const override { return 10; }
    bool init(ServerContext* ctx) override { ctx_ = ctx; return true; }
    bool start(ServerContext* ctx) override {
        if (!ctx->cfg.rssi.enabled.load()) {
            LOG_INFO(LogModule::RSSI, "RSSI monitor disabled by config");
            return true;
        }
        ctx->rssi_stop.store(false);
        start_rssi_monitor_thread(ctx, &worker_, "");  // ctrlDir 留空 → wpa_supplicant 默认路径
        return true;
    }
    void stop() override {
        if (!ctx_) return;
        ctx_->rssi_stop.store(true);
        if (worker_.joinable()) worker_.join();
    }
};

// ---------------------------------------------------------------------------
// TCP 丢包率监控（order 10）
// ---------------------------------------------------------------------------
class TcpLossPlugin : public IMonitorPlugin {
    ServerContext* ctx_ = nullptr;
    std::thread worker_;
public:
    const char* name() const override { return "tcp_loss"; }
    int order() const override { return 10; }
    bool init(ServerContext* ctx) override { ctx_ = ctx; return true; }
    bool start(ServerContext* ctx) override {
        if (!ctx->cfg.tcp_loss.enabled.load()) {
            LOG_INFO(LogModule::TCP_LOSS, "TCP loss monitor disabled by config");
            return true;
        }
        ctx->tcp_loss_stop.store(false);
        start_tcp_loss_monitor_thread(ctx, &worker_);
        return true;
    }
    void stop() override {
        if (!ctx_) return;
        ctx_->tcp_loss_stop.store(true);
        if (worker_.joinable()) worker_.join();
    }
};

// ---------------------------------------------------------------------------
// 流量分析（order 10：flow_rate.bpf.o 的持有者，先于其他 eBPF 消费插件）
// ---------------------------------------------------------------------------
class TrafficPlugin : public IMonitorPlugin {
    ServerContext* ctx_ = nullptr;
    std::thread worker_;
public:
    const char* name() const override { return "traffic"; }
    int order() const override { return 10; }
    bool init(ServerContext* ctx) override { ctx_ = ctx; return true; }
    bool start(ServerContext* ctx) override {
        if (!ctx->cfg.traffic.enabled.load()) {
            LOG_INFO(LogModule::WEAK_MGR, "Traffic analysis disabled by config");
            return true;
        }
        ctx->traffic_stop.store(false);
        start_traffic_analysis_thread(ctx, &worker_);
        return true;
    }
    void stop() override {
        if (!ctx_) return;
        ctx_->traffic_stop.store(true);
        if (worker_.joinable()) worker_.join();
    }
};

// ---------------------------------------------------------------------------
// 网络质量综合评估（order 10）
// ---------------------------------------------------------------------------
class QualityPlugin : public IMonitorPlugin {
    ServerContext* ctx_ = nullptr;
    std::thread worker_;
public:
    const char* name() const override { return "quality"; }
    int order() const override { return 10; }
    std::vector<std::string> dependencies() const override {
        return {"rtt", "jitter", "rssi", "tcp_loss", "traffic"};
    }
    bool init(ServerContext* ctx) override { ctx_ = ctx; return true; }
    bool start(ServerContext* ctx) override {
        if (!ctx->cfg.quality.enabled.load()) {
            LOG_INFO(LogModule::WEAK_MGR, "Network quality monitor disabled by config");
            return true;
        }
        ctx->quality_stop.store(false);
        start_network_quality_thread(ctx, &worker_);
        return true;
    }
    void stop() override {
        if (!ctx_) return;
        ctx_->quality_stop.store(true);
        if (worker_.joinable()) worker_.join();
    }
};

// ---------------------------------------------------------------------------
// 蓝牙监测（order 10）
// ---------------------------------------------------------------------------
class BluetoothPlugin : public IMonitorPlugin {
    ServerContext* ctx_ = nullptr;
    std::thread worker_;
    std::unique_ptr<BtMonitor> monitor_;
public:
    const char* name() const override { return "bluetooth"; }
    int order() const override { return 10; }
    bool init(ServerContext* ctx) override { ctx_ = ctx; return true; }
    bool start(ServerContext* ctx) override {
        if (!ctx->cfg.bluetooth.enabled.load()) {
            LOG_INFO(LogModule::BLUETOOTH, "Bluetooth monitor disabled by config");
            return true;
        }
        // 蓝牙监测器实例由插件创建并拥有；ServerContext 只保留查询用裸指针。
        monitor_ = std::make_unique<BtMonitor>();
        ctx->bt_monitor = monitor_.get();
        ctx->bluetooth_stop.store(false);
        start_bt_monitor_thread(ctx, &worker_, nullptr);
        return true;
    }
    void stop() override {
        if (!ctx_) return;
        ctx_->bluetooth_stop.store(true);
        if (worker_.joinable()) worker_.join();
        if (ctx_->bt_monitor == monitor_.get()) ctx_->bt_monitor = nullptr;
        monitor_.reset();
    }
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