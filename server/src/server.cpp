/**
 * @file server.cpp
 * @brief weaknet-dbus 服务端主逻辑 — 初始化、线程编排、优雅退出
 *
 * 本文件是整个服务端的核心编排层。start_server() 完成以下工作：
 *   1. Logger 初始化 → D-Bus 总线连接 + 请求服务名 → 注册对象路径
 *   2. 依次创建并启动所有监控线程（网卡、RTT、Jitter、RSSI、TCP丢包、
 *      流量分析、网络质量、蓝牙、以及 6 个 eBPF 监控器）
 *   3. 进入 Looper 事件循环（poll 模式监听 D-Bus fd + signal pipe）
 *   4. 收到 SIGINT/SIGTERM 后按依赖顺序 join 所有线程 → 释放资源 → 退出
 *
 * 线程安全注意：
 *   - ServerContext 是所有共享资源的生命周期容器，所有捕获 ctx* 的线程
 *     必须在 ctx 析构前 join 完成（start_server 的 join 序列保证这一点）
 *   - D-Bus 连接通过 dbus_threads_init_default() 支持多线程并发调用
 *   - eBPF 监控器实例由 ServerContext 持有 unique_ptr，线程仅 .get() 使用
 */

#include <dbus/dbus.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <sstream>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
#include <algorithm>
#include <mutex>
#include <csignal>
#include <net/if.h>

#include "common.hpp"
#include "network_epoch_store.hpp"
#include "serializer.hpp"
#include "weaknet_config.hpp"
#include "monitor_registry.hpp"
#include "net_iface.h"
#include "server.hpp"
#include "dbus_service.hpp"
#include "looper.hpp"
#include "net_info.hpp"
#include "weak_netmgr.hpp"
#include "rtt_monitor.hpp"
#include "rssi_monitor.hpp"
#include "tcp_loss_monitor.hpp"
#include "event_manager.hpp"
#include "logger.hpp"
#include "network_quality_result.hpp"
#include "assurance/legacy_adapter.hpp"
#include "assurance/overall_policy.hpp"
#include "assurance/state_stabilizer.hpp"
#include "assurance/ip_reachability_evaluator.hpp"
#include "assurance/responsiveness_evaluator.hpp"
#include "assurance/reliability_evaluator.hpp"
#include "assurance/rf_health_evaluator.hpp"
#include "bt_monitor.hpp"
#include "band_conflict_detector.hpp"
#include "bt_audio_fusion.hpp"
#include "dns_monitor.hpp"
#include "assurance/dns_transaction_tracker.hpp"
#include "assurance/dns_service_evaluator.hpp"
#include "wifi_packet_loss_monitor.hpp"
#include "http_latency_monitor.hpp"
#include "process_net_profiler.hpp"
#include "tcp_retransmit_monitor.hpp"
#include "tcp_conn_monitor.hpp"
#include "tcp_connect_monitor.hpp"
#include "assurance/tcp_connect_evaluator.hpp"
#include "assurance/active_connectivity.hpp"
#include "active_connectivity_monitor.hpp"
#include "assurance/http_access_evaluator.hpp"
#include "assurance/captive_portal_evaluator.hpp"
#include "http_latency_monitor.hpp"
#include "bt_audio_analyzer.hpp"
#include "database_manager.hpp"
#include "using_iface.h"
#include "net_wifiriss.h"
#include <iomanip>
#include <map>
#include "metrics/metric_normalizer.hpp"

using namespace std::chrono_literals;

namespace weaknet_dbus {

// ServerContext 析构：释放 DBus 连接与 service/weak_mgr 资源。
// 调用前提：所有捕获 ctx* 的监控线程已在 start_server() 退出路径完成 join，
// 之后才进入本析构，避免在回收线程访问尚未析构的成员。
ServerContext::~ServerContext() {
    // 注意：所有捕获 ctx* 的监控线程必须在调用此析构函数前已完成 join。
    // start_server() 通过 join_all → ctx 离开作用域的顺序保证这一点。

    if (connection) {
        // 共享连接（dbus_bus_get 获取）不应调用 dbus_connection_close，
        // 只需 unref 释放引用。close 会导致 d-bus 守护进程报错 "Application
        // must not close shared connections"。
        dbus_connection_unref(connection);
        connection = nullptr;
    }
    // 智能指针自动释放，无需手动 delete
}

// 共享列表迁移至 ServerContext，在 server.hpp 中定义

// 使用 DbusService 替代手写处理函数

// 将字符串作为方法返回，通过序列化保存到文件
// Get 方法处理已迁移到 DbusService

// 发送 Changed 信号，并将载荷序列化到文件
// 发信号也迁移到 DbusService

// 处理进入总线的消息（方法调用等）
// 消息处理迁移，由 DbusService::MessageHandler 提供

// 将字符串数组作为返回，已封装到 DbusService

// 比较两个列表，打印新增与删除项，并返回是否有变化
static bool diffInterfaces(const std::vector<std::string>& old_list,
                           const std::vector<std::string>& new_list,
                           std::vector<std::string>& added,
                           std::vector<std::string>& removed) {
    added.clear();
    removed.clear();

    for (const auto& it : new_list) {
        if (std::find(old_list.begin(), old_list.end(), it) == old_list.end()) added.push_back(it);
    }
    for (const auto& it : old_list) {
        if (std::find(new_list.begin(), new_list.end(), it) == new_list.end()) removed.push_back(it);
    }
    return !added.empty() || !removed.empty();
}

// 独立接口：初始化 DBus、注册对象路径
DBusConnection* init_dbus(ServerContext* ctx) {
    LOG_INFO(LogModule::DBUS, "init_dbus: start connecting to system bus...");
    dbus_threads_init_default();

    DBusError err;
    dbus_error_init(&err);

    // 服务端以 root 系统服务运行（eBPF 需要 CAP_BPF/CAP_SYS_ADMIN），
    // 用户会话总线的生命周期与桌面/SSH 会话绑定且不允许 root 连接，
    // 因此统一使用系统总线（与 bt_monitor 访问 BlueZ 的拓扑一致）。
    DBusConnection* conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
    if (dbus_error_is_set(&err)) {
        LOG_ERROR(LogModule::DBUS, "连接总线失败: " << err.message);
        dbus_error_free(&err);
    }
    if (!conn) return nullptr;
    LOG_INFO(LogModule::DBUS, "connected to system bus");

    LOG_INFO(LogModule::DBUS, "requesting bus name: " << kBusName);
    int ret = dbus_bus_request_name(conn, kBusName, DBUS_NAME_FLAG_REPLACE_EXISTING, &err);
    if (dbus_error_is_set(&err)) {
        LOG_ERROR(LogModule::DBUS, "请求服务名失败: " << err.message);
        dbus_error_free(&err);
    }
    if (ret != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
        LOG_ERROR(LogModule::DBUS, "未能成为主拥有者，ret=" << ret);
        dbus_connection_unref(conn);
        return nullptr;
    }

    // 使用服务类进行对象注册（保存到上下文，统一管理生命周期）
    LOG_INFO(LogModule::DBUS, "registering object path: " << kObjectPath << " (interface=" << kInterface << ")");
    ctx->service = std::make_unique<DbusService>(ctx);
    if (!ctx->service->register_on_connection(conn)) {
        LOG_ERROR(LogModule::DBUS, "注册对象路径失败");
        ctx->service.reset();
        dbus_connection_unref(conn);
        return nullptr;
    }
    // 指针已保存至 ctx

    ctx->connection = conn;
    LOG_INFO(LogModule::DBUS, "DBus 服务端已启动，接口 " << kInterface << "，方法 " << kMethodGet << "，信号 " << kSignalChanged);
    return conn;
}

// 独立接口：启动网卡监控线程（使用 WeakNetMgr 与 NetInfo）
void start_iface_monitor_thread(ServerContext* ctx, std::thread* worker) {
    *worker = std::thread([ctx](){
        LOG_INFO(LogModule::INTERFACE, "monitor thread started");
        std::vector<NetInfo> current;
        int32_t change_counter = 0;

        while ((ctx->running.load() && !ctx->iface_stop.load())) {
            LOG_INFO(LogModule::INTERFACE, "tick: collecting interfaces...");
            std::vector<NetInfo> latest = ctx->weak_mgr->collectCurrentInterfaces();
            LOG_INFO(LogModule::INTERFACE, "collected " << latest.size() << " interfaces");
            
            // 持续输出关键指标信息
            for (const auto& net : latest) {
                if (net.usingNow()) {
                    LOG_INFO(LogModule::INTERFACE, "ACTIVE: " << net.ifName() 
                        << " | RTT: " << net.rttMs() << "ms" 
                        << " | Jitter: " << net.jitterMs() << "ms (" << net.jitterLevel() << ")"
                        << " | Quality: " << static_cast<int>(net.quality())
                        << " | RSSI: " << net.rssiDbm() << "dBm"
                        << " | TCP Loss: " << net.tcpLossRate() << "% (" << net.tcpLossLevel() << ")"
                        << " | Traffic: " << (net.trafficTotalBps() / (1024*1024)) << "MB/s, " 
                        << net.trafficActiveFlows() << " flows, " << net.trafficTotalPps() << " pps");
                } else {
                    LOG_INFO(LogModule::INTERFACE, "INACTIVE: " << net.ifName() 
                        << " | RTT: " << net.rttMs() << "ms" 
                        << " | Jitter: " << net.jitterMs() << "ms (" << net.jitterLevel() << ")"
                        << " | Quality: " << static_cast<int>(net.quality())
                        << " | RSSI: " << net.rssiDbm() << "dBm"
                        << " | TCP Loss: " << net.tcpLossRate() << "% (" << net.tcpLossLevel() << ")");
                }
            }
            auto old_names = WeakNetMgr::namesOf(current);
            auto new_names = WeakNetMgr::namesOf(latest);
            std::vector<std::string> added, removed;
            if (diffInterfaces(old_names, new_names, added, removed)) {
                current = latest;
                // 使用线程安全的方法更新接口列表
                ctx->weak_mgr->updateInterfaces(current);
                std::string msg = "Interfaces changed (using flags in log): +";
                for (size_t i = 0; i < added.size(); ++i) { msg += (i == 0 ? "" : ","); msg += added[i]; }
                msg += " -";
                for (size_t i = 0; i < removed.size(); ++i) { msg += (i == 0 ? "" : ","); msg += removed[i]; }
                LOG_INFO(LogModule::INTERFACE, msg);
                // 打印 using 标志
                for (const auto& x : current) {
                    if (x.usingNow()) {
                        LOG_INFO(LogModule::INTERFACE, "[using] " << x.ifName() << " is current uplink");
                    }
                }
                // 同步发射（内部有锁），不创建 detached 子线程；事件更新经 EventManager 同步
                if (ctx->service) {
                    ctx->service->emitChanged(msg, change_counter);
                    getEventManager().emitInterfaceChanged(msg, "network_manager");
                }
            } else {
                LOG_INFO(LogModule::INTERFACE, "no changes detected");
            }
            for (int i = 0; i < 100 && (ctx->running.load() && !ctx->iface_stop.load()); ++i)
                std::this_thread::sleep_for(100ms);
        }
    });
}

// 独立接口：启动流量分析线程
void start_traffic_analysis_thread(ServerContext* ctx, std::thread* worker) {
    *worker = std::thread([ctx](){
        LOG_INFO(LogModule::WEAK_MGR, "traffic analysis thread started");

        // 动态选择接口：优先使用配置，否则取当前活动接口
        std::string targetIface = kDefaultTrafficInterface;
        if (targetIface.empty()) {
            // 从 WeakNetMgr 获取当前活动接口
            auto interfaces = ctx->weak_mgr->getCurrentInterfaces();
            for (const auto& iface : interfaces) {
                if (iface.usingNow()) {
                    targetIface = iface.ifName();
                    break;
                }
            }
            if (targetIface.empty()) {
                targetIface = "wlan0";  // 兜底默认值
            }
        }

        LOG_INFO(LogModule::WEAK_MGR, "traffic analysis: using interface " << targetIface);
        ctx->weak_mgr->startTrafficAnalysis(targetIface, ctx->cfg.traffic.interval_ms.load() / 1000);

        int loop_count = 0;
        while ((ctx->running.load() && !ctx->traffic_stop.load())) {
            loop_count++;
            LOG_INFO(LogModule::WEAK_MGR, "traffic analysis thread running, loop=" << loop_count);
            try {
                // 直接调用线程安全的流量分析更新方法
                LOG_INFO(LogModule::WEAK_MGR, "traffic analysis: calling updateTrafficAnalysisSafe");
                bool changed = ctx->weak_mgr->updateTrafficAnalysisSafe();
                LOG_INFO(LogModule::WEAK_MGR, "traffic analysis: updateTrafficAnalysisSafe completed, changed=" << changed);

                // 获取当前接口列表用于日志输出
                auto current_interfaces = ctx->weak_mgr->getCurrentInterfaces();
                LOG_INFO(LogModule::WEAK_MGR, "traffic analysis: current interfaces count=" << current_interfaces.size());

                if (changed && ctx->service) {
                    LOG_INFO(LogModule::WEAK_MGR, "Traffic analysis updated - emitting signal");
                    // 同步发射（内部有锁），不创建 detached 子线程
                    ctx->service->emitChanged("Traffic analysis updated", /*counter*/0);
                } else {
                    LOG_INFO(LogModule::WEAK_MGR, "TRAFFIC_ANALYSIS: no changes detected (interfaces: " << current_interfaces.size() << ")");
                }
            } catch (const std::exception& e) {
                LOG_ERROR(LogModule::WEAK_MGR, "Traffic analysis error: " << e.what());
            }

            for (int i = 0; i < static_cast<int>(ctx->cfg.traffic.interval_ms.load() / 100) && (ctx->running.load() && !ctx->traffic_stop.load()); ++i)
                std::this_thread::sleep_for(100ms);
        }
        ctx->weak_mgr->stopTrafficAnalysis();
        LOG_INFO(LogModule::WEAK_MGR, "traffic analysis thread stopped");
    });
}

// 独立接口：启动"当前上网网卡"监控线程（使用 UsingInterfaceManager）
void start_using_iface_thread(ServerContext* ctx, std::thread* worker) {
    *worker = std::thread([ctx](){
        LOG_INFO(LogModule::WEAK_MGR, "monitor thread started");

        int loop_count = 0;
        while ((ctx->running.load() && !ctx->using_iface_stop.load())) {
            loop_count++;
            LOG_INFO(LogModule::WEAK_MGR, "using iface thread running, loop=" << loop_count);
            
            // 直接调用线程安全的当前使用接口更新方法
            LOG_INFO(LogModule::WEAK_MGR, "using iface: calling updateCurrentUsingSafe");
            bool changed = ctx->weak_mgr->updateCurrentUsingSafe();
            LOG_INFO(LogModule::WEAK_MGR, "using iface: updateCurrentUsingSafe completed, changed=" << changed);
            
            // 获取当前接口列表用于日志输出
            auto current_interfaces = ctx->weak_mgr->getCurrentInterfaces();
            LOG_INFO(LogModule::WEAK_MGR, "using iface: current interfaces count=" << current_interfaces.size());
            
            if (changed && ctx->service) {
                // 查找当前使用的接口
                std::string currentIf;
                for (const auto& net : current_interfaces) {
                    if (net.usingNow()) {
                        currentIf = net.ifName();
                        break;
                    }
                }
                
                std::string msg = std::string("Using iface updated: ") + (currentIf.empty() ? "(none)" : currentIf);
                // 同步发射（内部有锁），不创建 detached 子线程
                ctx->service->emitChanged(msg, /*counter*/0);
                getEventManager().emitConnectionModeChanged(msg, currentIf.empty() ? "none" : currentIf);
            } else {
                LOG_INFO(LogModule::WEAK_MGR, "unchanged (interfaces: " << current_interfaces.size() << ")");
            }
            for (int i = 0; i < 100 && (ctx->running.load() && !ctx->using_iface_stop.load()); ++i)
                std::this_thread::sleep_for(100ms);
        }
    });
}

// 独立接口：启动网络质量监控线程
void start_network_quality_thread(ServerContext* ctx, std::thread* worker) {
    *worker = std::thread([ctx](){
        LOG_INFO(LogModule::WEAK_MGR, "network assurance engine thread started (Phase 1)");

        weaknet::StateStabilizer stabilizer;
        weaknet::HealthState lastStableState = weaknet::HealthState::UNKNOWN;

        int loop_count = 0;
        while ((ctx->running.load() && !ctx->quality_stop.load())) {
            loop_count++;
            try {
                // 无上行网卡时**不编造**一张网卡来评估：宁可本轮跳过并给出
                // 显式日志，也不要把某个不相关网卡的指标当成"当前网络体验"。
                // 此前默认 "wlan0" 再被列表首位覆盖，会在启动早期/路由抖动期间
                // 产出一份与实际链路无关的权威快照。
                std::string active_iface;
                if (ctx->weak_mgr) {
                    auto opt = ctx->weak_mgr->getCurrentUsingInterface();
                    if (opt.has_value()) active_iface = opt.value();
                }
                if (active_iface.empty()) {
                    LOG_WARNING(LogModule::WEAK_MGR,
                                "no interface is currently using the network; "
                                "skipping this assessment round (no fabricated iface)");
                    // 与循环尾部同样的 100ms 分片等待：整段睡眠会让停止请求
                    // 最长等待一个完整 interval 才被响应。
                    for (int i = 0;
                         i < static_cast<int>(ctx->cfg.quality.interval_ms.load() / 100)
                             && (ctx->running.load() && !ctx->quality_stop.load());
                         ++i) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }
                    continue;
                }

                weaknet::NetworkExperience exp;
                uint64_t newest_rev = 0;
                bool dns_bypass = false;

                if (ctx->metrics_registry) {
                    using namespace std::chrono_literals;
                    auto reach_samples = ctx->metrics_registry->window(active_iface, weaknet::MetricId::REACHABILITY_SUCCESS, 120s);
                    auto rtt_samples = ctx->metrics_registry->window(active_iface, weaknet::MetricId::RTT_MS, 120s);
                    auto jitter_samples = ctx->metrics_registry->window(active_iface, weaknet::MetricId::JITTER_MS, 120s);
                    auto wifi_samples = ctx->metrics_registry->window(active_iface, weaknet::MetricId::WIFI_LOSS_RATE, 120s);
                    auto tcp_samples = ctx->metrics_registry->window(active_iface, weaknet::MetricId::TCP_LOSS_RATE, 120s);
                    auto rssi_samples = ctx->metrics_registry->window(active_iface, weaknet::MetricId::RSSI_DBM, 120s);

                    bool is_wireless = (active_iface.rfind("wl", 0) == 0);
                    auto reach_sle = weaknet::IpReachabilityEvaluator::evaluate(reach_samples);
                    auto resp_sle = weaknet::ResponsivenessEvaluator::evaluate(rtt_samples, jitter_samples);
                    auto rel_sle = weaknet::ReliabilityEvaluator::evaluate(wifi_samples, tcp_samples, is_wireless);
                    auto rf_sle = weaknet::RfHealthEvaluator::evaluate(rssi_samples, is_wireless);
                    weaknet::SleResult dns_sle;
                    if (ctx->dns_tracker) {
                        auto snap = ctx->dns_tracker->getSnapshot();
                        auto dns_window = ctx->dns_tracker->getWindowMetrics(120s, snap.cutoff);
                        dns_sle = weaknet::DnsServiceEvaluator::evaluate(
                            dns_window, ctx->dns_tracker->getRecentTerminals(), {}, &dns_bypass);
                        // DNS SLE 的判定依据与结果必须可观测：否则"DNS 为什么是这个状态"
                        // 无法回答，排障只能靠猜。这里输出窗口计数与决策结果。
                        LOG_INFO(LogModule::NETWORK, "DNS SLE: state="
                            << weaknet::healthStateToString(dns_sle.state)
                            << " coverage=" << weaknet::coverageToString(dns_sle.coverage)
                            << " reason=" << dns_sle.reason
                            << " fail=" << dns_window.knownFailure() << "/" << dns_window.evaluableTerminals()
                            << " inflight=" << dns_window.current_inflight
                            << " ok=" << dns_window.knownSuccess()
                            << " unmatched=" << dns_window.unmatched
                            << " ambiguous=" << dns_window.tracking_ambiguous
                            << " late=" << dns_window.late_responses);
                    }

                    // TCP Connect SLE：回答"DNS 解析出地址后，能否真的建立连接"。
                    // 与 DNS 同源纪律：证据不足 → UNKNOWN，观测不可靠 → UNKNOWN。
                    weaknet::SleResult tcp_sle;
                    if (ctx->tcp_connect_monitor) {
                        weaknet::TcpConnectEvaluator::Input tcp_in;
                        for (const auto& o : ctx->tcp_connect_monitor->recentObservations()) {
                            tcp_in.samples.push_back({o.success, o.latency_ms});
                        }
                        const auto tcp_stats = ctx->tcp_connect_monitor->stats();
                        tcp_in.unmatched_terminal = tcp_stats.unmatched_terminal;
                        // 捕获事件量以窗口内样本为准（内核计数器为累计值，不直接用于比率）
                        tcp_in.capture_events = tcp_in.samples.size() + tcp_stats.unmatched_terminal;
                        tcp_sle = weaknet::TcpConnectEvaluator::evaluate(tcp_in);
                        LOG_INFO(LogModule::NETWORK, "TCP SLE: state="
                            << weaknet::healthStateToString(tcp_sle.state)
                            << " coverage=" << weaknet::coverageToString(tcp_sle.coverage)
                            << " reason=" << tcp_sle.reason
                            << " ok=" << tcp_stats.successes << " fail=" << tcp_stats.failures
                            << " unmatched=" << tcp_stats.unmatched_terminal);
                    }

                    // HTTP/HTTPS Access SLE：TCP 通了不代表应用层可用。
                    // 语义边界：4xx 是业务语义（服务端正常应答），只有 5xx 与
                    // 无响应/TLS 失败才算传输层故障。
                    weaknet::SleResult http_sle;
                    weaknet::SleResult portal_sle;
                    if (ctx->http_latency_monitor) {
                        weaknet::HttpAccessEvaluator::Input http_in;
                        const auto txns = ctx->http_latency_monitor->getRecentTxns(200);
                        for (const auto& t : txns) {
                            http_in.samples.push_back({t.statusCode, static_cast<double>(t.ttfbNs) / 1e6, false});
                        }
                        http_in.capture_events = http_in.samples.size();
                        http_sle = weaknet::HttpAccessEvaluator::evaluate(http_in);

                        // Captive Portal：**当前不具备可靠判定能力**。
                        //
                        // 可靠的 portal 判定需要受控探测（向已知 connectivity-check
                        // 端点请求，看是否被重定向/内容替换）。当前 capture 不提取
                        // Location 头，也没有主动探测。
                        // 普通 301/302 是网站常见正常行为，绝不能等价于门户。
                        // 因此这里不喂入任何被动重定向作为判定依据，
                        // evaluator 会如实返回 UNKNOWN / NO_CAPABILITY。
                        weaknet::CaptivePortalEvaluator::Input portal_in;
                        portal_in.has_controlled_probe = false;
                        portal_in.ip_reachable = (reach_sle.state == weaknet::HealthState::GOOD);
                        portal_in.dns_resolvable = (dns_sle.state == weaknet::HealthState::GOOD);
                        portal_in.tcp_connectable = (tcp_sle.state == weaknet::HealthState::GOOD);
                        portal_sle = weaknet::CaptivePortalEvaluator::evaluate(portal_in);

                        LOG_INFO(LogModule::NETWORK, "HTTP SLE: state="
                            << weaknet::healthStateToString(http_sle.state)
                            << " coverage=" << weaknet::coverageToString(http_sle.coverage)
                            << " reason=" << http_sle.reason
                            << " samples=" << txns.size()
                            << " | Portal: " << weaknet::healthStateToString(portal_sle.state)
                            << " reason=" << portal_sle.reason);
                    }

                    // 受控主动探测：host-level Internet 能力的唯一证据来源。
                    // 与被动观测分开评估、分开传入 —— 两者语义不同，
                    // 绝不混入同一统计窗口。
                    weaknet::ActiveConnectivityResult active;
                    if (ctx->active_probe) {
                        // 能力声明取自编译期事实，而不是从结果推断：
                        // 未声明能力（tls_available=false）时 evaluator
                        // 必须返回 NO_CAPABILITY，绝不能因为恰好有 TLS 数据
                        // 就宣称 HTTPS 可用。
                        weaknet::ActiveConnectivityConfig acfg;
                        acfg.tls_available = ctx->cfg.active_probe.https_enabled.load()
                                             && ActiveConnectivityMonitor::tlsAvailable();
                        // 底层健康度：Core SLE 决定。Portal 判定要求底层健康，
                        // 否则把"网断了"误归因成"被门户拦截"。
                        acfg.underlying_healthy =
                            reach_sle.state != weaknet::HealthState::BAD
                            && rel_sle.state != weaknet::HealthState::BAD
                            && dns_sle.state != weaknet::HealthState::BAD;
                        active = weaknet::ActiveConnectivityEvaluator::evaluate(
                            ctx->active_probe->results(),
                            ctx->active_probe->isUsable(),
                            acfg);
                        // Portal 使用独立的受控 oracle 结果
                        if (acfg.tls_available) {
                            const auto portal_targets = ctx->active_probe->portalResults();
                            if (!portal_targets.empty()) {
                                auto pr = weaknet::ActiveConnectivityEvaluator::evaluate(
                                    portal_targets, ctx->active_probe->isUsable(), acfg);
                                active.portal = pr.portal;
                                active.portal_suspected = pr.portal_suspected;
                            }
                        }
                        LOG_INFO(LogModule::NETWORK, "Active capability: dns="
                            << weaknet::healthStateToString(active.dns.state)
                            << "(" << active.dns.reason << ") tcp="
                            << weaknet::healthStateToString(active.tcp.state)
                            << "(" << active.tcp.reason << ") https="
                            << weaknet::healthStateToString(active.https.state)
                            << "(" << active.https.reason << ") portal="
                            << weaknet::healthStateToString(active.portal.state)
                            << "(" << active.portal.reason << ")");
                    }

                    exp = weaknet::OverallPolicy::decide(active_iface, reach_sle, resp_sle, rel_sle,
                                                         rf_sle, dns_sle, tcp_sle,
                                                         http_sle, portal_sle,
                                                         active.dns, active.tcp,
                                                         active.https, active.portal,
                                                         currentAssessmentProfile(*ctx));
                    if (!rtt_samples.empty()) newest_rev = rtt_samples.back().sequence;
                    else if (!reach_samples.empty()) newest_rev = reach_samples.back().sequence;
                } else {
                    exp.iface = active_iface;
                    exp.overall = weaknet::HealthState::UNKNOWN;
                    exp.display_score = 50;
                }

                // CR-2: 状态防抖（只有出现新 evidence 时才推进，发生稳定跃迁时发射信号）
                // SR-9 单杀时传 is_critical_bypass=true，立即穿透防抖滞后
                weaknet::HealthState stableState = stabilizer.update(
                    exp.overall, newest_rev, std::chrono::steady_clock::now(), dns_bypass);
                exp.overall = stableState;

                // W2: 发布权威评估快照（单一事实源）。
                // quality 线程是唯一 evaluator 执行点；HealthCheck /
                // GetNetworkExperience / history persistence 全部只读本快照。
                {
                    auto snap = std::make_shared<weaknet::AssessmentSnapshot>();
                    snap->sequence_id = ++ctx->assessment_sequence; // 每轮都发布（含 UNKNOWN），消费者可见评估节奏
                    snap->evaluated_at_monotonic = std::chrono::steady_clock::now();
                    snap->wall_timestamp = std::chrono::system_clock::now();
                    snap->profile = currentAssessmentProfile(*ctx);
                    snap->config_generation = ctx->cfg.config_generation.load();
                    snap->network_epoch = ctx->dns_tracker ? ctx->dns_tracker->currentBindingEpoch() : ctx->dns_binding_epoch.load();
                    snap->experience = exp;
                    // 以 const 指针发布：读侧拿到后不可修改（不可变快照语义）
                    ctx->assessment_store.publish(std::const_pointer_cast<const weaknet::AssessmentSnapshot>(std::move(snap)));
                }

                // 边缘遥测上报（可选，W-edge）：
                // publish 之后把同一份快照交给上报器（非阻塞，O(1) enqueue）。
                // 与 history/D-Bus 消费者一样只读已发布的不可变快照，
                // 绝不重新 evaluate；enqueue 失败绝不影响评估主循环。
                if (ctx->edge_exporter && ctx->edge_exporter->isRunning()) {
                    auto latest = ctx->assessment_store.latest();
                    if (latest) {
                        // 传 shared_ptr 而非 *latest：exporter 需要共同持有该快照，
                        // 否则本作用域结束后它会留下悬垂指针（详见 enqueue 声明处）。
                        ctx->edge_exporter->enqueue(std::move(latest));
                    }
                }

                if (stableState != lastStableState) {
                    auto qualRes = weaknet::LegacyAdapter::toQualityResult(exp);
                    LOG_INFO(LogModule::WEAK_MGR, "网络质量稳定跃迁: " << qualRes.levelName
                        << " (分数: " << std::fixed << std::setprecision(1) << qualRes.score << ")");

                    getEventManager().emitNetworkQualityChanged(
                        qualRes.levelName,
                        qualRes.details,
                        "network_quality_assessor"
                    );
                    lastStableState = stableState;
                }
            } catch (const std::exception& e) {
                LOG_ERROR(LogModule::WEAK_MGR, "网络质量监控错误: " << e.what());
            }

            // ================================================================
            // 2.4GHz 频段冲突检测（Phase 1a）
            // 关联 Wi-Fi RSSI 与蓝牙 RSSI，识别共享频段干扰
            // ================================================================
            try {
                // 使用 thread_local 确保检测器状态在循环间保持
                static thread_local BandConflictDetector conflictDetector;

                // 获取当前上网接口的 Wi-Fi RSSI 与频段感知
                int wifiRssi = -1000;
                std::string activeWifiIface;
                {
                    auto interfaces = ctx->weak_mgr->getCurrentInterfaces();
                    for (const auto& iface : interfaces) {
                        if (iface.usingNow() && iface.hasRssi()) {
                            wifiRssi = iface.rssiDbm();
                            activeWifiIface = iface.ifName();
                            break;
                        }
                    }
                    // 若无 usingNow 接口，退而取第一个有 RSSI 的 Wi-Fi 接口
                    if (wifiRssi <= -1000) {
                        for (const auto& iface : interfaces) {
                            if (iface.hasRssi()) {
                                wifiRssi = iface.rssiDbm();
                                activeWifiIface = iface.ifName();
                                break;
                            }
                        }
                    }
                }

                // 探测当前 Wi-Fi 工作频段（2.4GHz / 5GHz / 6GHz）
                std::string wifiBand = "2.4GHz";
                if (!activeWifiIface.empty()) {
                    auto wifiClient = WiFiRssiClient::getInstance();
                    if (wifiClient) {
                        int freq = wifiClient->getFrequency();
                        if (freq >= 5000 && freq <= 5900) {
                            wifiBand = "5GHz";
                        } else if (freq >= 5925 && freq <= 7125) {
                            wifiBand = "6GHz";
                        } else if (freq >= 2400 && freq <= 2500) {
                            wifiBand = "2.4GHz";
                        }
                    }
                }

                // 获取蓝牙 RSSI（取所有已连接设备的平均 RSSI）
                int btRssi = -1000;
                if (auto* mon = ctx->bt_monitor; mon && mon->isInitialized()) {
                    auto rssiSnapshot = mon->getRssiSnapshot();
                    int sum = 0, count = 0;
                    for (const auto& [mac, rssi] : rssiSnapshot) {
                        if (rssi != 0 && rssi > -1000) {
                            sum += rssi;
                            ++count;
                        }
                    }
                    if (count > 0) {
                        btRssi = sum / count;
                    }
                }

                // 推入样本并检测
                if (wifiRssi > -1000 && btRssi > -1000) {
                    conflictDetector.feedSample(wifiRssi, btRssi);
                }

                auto conflictResult = conflictDetector.detect(wifiBand);
                conflictResult.wifiIface = activeWifiIface;
                {
                    std::lock_guard<std::mutex> lock(ctx->conflict_mutex);
                    std::ostringstream oss;
                    oss << "{"
                        << "\"detected\":" << (conflictResult.detected ? "true" : "false") << ","
                        << "\"confidence\":" << conflictResult.confidence << ","
                        << "\"correlation\":" << conflictResult.correlation << ","
                        << "\"wifi_rssi_drop\":" << conflictResult.wifiRssiDrop << ","
                        << "\"bt_rssi_drop\":" << conflictResult.btRssiDrop << ","
                        << "\"wifi_iface\":\"" << conflictResult.wifiIface << "\","
                        << "\"wifi_band\":\"" << conflictResult.wifiBand << "\","
                        << "\"bt_mac\":\"" << conflictResult.btMac << "\","
                        << "\"bt_audio_active\":" << (conflictResult.btAudioActive ? "true" : "false") << ","
                        << "\"suggestion\":\"" << conflictResult.suggestion << "\""
                        << "}";
                    ctx->latest_conflict_json = oss.str();
                }

                if (conflictResult.detected && conflictResult.confidence > 50.0) {
                    LOG_INFO(LogModule::WEAK_MGR,
                             "2.4GHz band conflict detected: confidence="
                             << conflictResult.confidence
                             << "%, correlation=" << conflictResult.correlation
                             << ", wifiDrop=" << conflictResult.wifiRssiDrop
                             << "dBm, btDrop=" << conflictResult.btRssiDrop << "dBm");

                    getEventManager().emitNetworkQualityChanged(
                        "2.4GHz band conflict detected",
                        conflictResult.suggestion,
                        "band_conflict_detector"
                    );
                }
            } catch (const std::exception& e) {
                LOG_ERROR(LogModule::WEAK_MGR,
                          "频段冲突检测错误: " << e.what());
            }

            // ================================================================
            // Phase 2: 蓝牙音频质量融合评估
            // 融合 D-Bus MediaTransport1 状态 + eBPF L2CAP 流量统计
            // 可检测 "active 但卡顿" 状态，eBPF 不可用时自动降级
            // ================================================================
            try {
                BtMonitor* mon = ctx->bt_monitor;
                if (mon && mon->isInitialized()) {
                    auto connected = mon->getConnectedDevices();
                    for (const auto& dev : connected) {
                        BtAudioFusionResult fusionResult;
                        if (mon->getAudioFusionResult(dev.macAddress, &fusionResult)) {
                            // 仅在有异常时输出日志（减少正常情况下的日志量）
                            if (fusionResult.suspectedStall) {
                                LOG_WARNING(LogModule::BLUETOOTH,
                                    "BT_AUDIO_STALL: " << dev.macAddress
                                    << " (" << (dev.name.empty() ? "unknown" : dev.name) << ")"
                                    << " | score=" << fusionResult.qualityScore
                                    << " | level=" << fusionResult.level
                                    << " | maxGap=" << fusionResult.maxGapMs << "ms"
                                    << " | " << fusionResult.diagnostic);

                                getEventManager().emitBluetoothDeviceChanged(
                                    "Audio stall suspected: " + dev.macAddress
                                    + " score=" + std::to_string(static_cast<int>(fusionResult.qualityScore))
                                    + " " + fusionResult.diagnostic,
                                    dev.name.empty() ? dev.macAddress : dev.name);
                            } else if (fusionResult.qualityScore < 60.0) {
                                // 低质量但不一定是卡顿
                                LOG_INFO(LogModule::BLUETOOTH,
                                    "BT_AUDIO_LOW: " << dev.macAddress
                                    << " | score=" << fusionResult.qualityScore
                                    << " | level=" << fusionResult.level
                                    << " | " << fusionResult.diagnostic);
                            }
                        }
                    }
                }
            } catch (const std::exception& e) {
                LOG_ERROR(LogModule::WEAK_MGR,
                          "Phase 2 audio fusion error: " << e.what());
            }
            
            for (int i = 0; i < static_cast<int>(ctx->cfg.quality.interval_ms.load() / 100) && (ctx->running.load() && !ctx->quality_stop.load()); ++i)
                std::this_thread::sleep_for(100ms);
        }

        LOG_INFO(LogModule::WEAK_MGR, "network quality monitor thread stopped");
    });
}

// ====================================================================
// eBPF 监控器线程启动函数
// 将此前孤立的 BPF 监控器纳入 ServerContext 统一生命周期
// ====================================================================

void start_dns_monitor_thread(ServerContext* ctx, std::thread* worker, DnsMonitor* monitor) {
    // 监控器由 ServerContext 持有 ownership（unique_ptr），线程仅通过 .get() 使用。
    // 这消除了旧方案中「线程销毁 unique_ptr 后、store(nullptr) 前」的悬垂指针窗口。
    *worker = std::thread([ctx, monitor]() {
        // The plugin owns this monitor; the worker borrows it until join.
        if (!monitor) return;
        LOG_INFO(LogModule::NETWORK, "DNS monitor thread started");

        constexpr int kTickMs = 100;         // 循环粒度
        constexpr int kSweepEveryTicks = 10; // 每 1s 扫描一次超时（约等于 query_timeout 的分辨率需求）
        int tick_in_interval = 0;            // 位于当前 interval 内的第几个 tick
        int sweep_accum = 0;                 // 距上次 sweep 的 tick 数
        int diag_accum = 0;                  // 距上次 diag 的 tick 数

        while ((ctx->running.load() && !ctx->dns_stop.load())) {
            if (ctx->dns_monitor && ctx->dns_tracker) {
                // perf buffer 每 tick 排空一次。原先只在 interval 边界排空一次，
                // 导致突发 syscall 在 10s 内填满 ring buffer 而丢事件。
                auto drained = ctx->dns_monitor->drainEvents(ctx->dns_tracker.get());
                if (drained > 0) {
                    LOG_INFO(LogModule::NETWORK, "DNS events drained=" << drained);
                }

                if (++sweep_accum >= kSweepEveryTicks) {
                    sweep_accum = 0;
                    ctx->dns_tracker->sweepTimeouts();
                    // 观测质量增量随 sweep 节奏（每秒）汇入，避免每 tick 都读 BPF map。
                    ctx->dns_monitor->feedTransportDelta(ctx->dns_tracker.get());
                }

                const int diag_every_ticks = std::max(1, static_cast<int>(ctx->cfg.dns.interval_ms.load() / kTickMs));
                if (++diag_accum >= diag_every_ticks) {
                    diag_accum = 0;
                    LOG_INFO(LogModule::NETWORK, "dns-capture diag: "
                        << ctx->dns_monitor->getCaptureDiagnostics());
                }
            }

            // interval 边界：输出聚合 tick 日志（保持原有观测节奏）
            if (++tick_in_interval * kTickMs >= static_cast<int>(ctx->cfg.dns.interval_ms.load())) {
                tick_in_interval = 0;
                auto stats = monitor->getStats();
                if (stats.totalQueries > 0) {
                    LOG_INFO(LogModule::NETWORK, "DNS tick: queries=" << stats.totalQueries
                        << " avgLatency=" << stats.avgLatencyMs << "ms"
                        << " timeoutRate=" << stats.timeoutRate() << "%");
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(kTickMs));
        }
        monitor->stop();
        LOG_INFO(LogModule::NETWORK, "DNS monitor thread stopped");
    });
}

void start_wifi_loss_monitor_thread(ServerContext* ctx, std::thread* worker, WifiPacketLossMonitor* monitor) {
    *worker = std::thread([ctx, monitor]() {
        // The plugin owns this monitor; the worker borrows it until join.
        if (!monitor) return;
        LOG_INFO(LogModule::NETWORK, "Wi-Fi loss monitor thread started");
        // 每接口独立的累计计数器归一化器：把驱动层 since-boot 计数器转成
        // 采样周期增量丢包率。处理首轮基线、计数器倒退（probe 重载/网卡重置）
        // 与零活动（无发包周期不产出伪 rate）。
        std::map<uint32_t, weaknet::CounterNormalizer> normalizers;
        while ((ctx->running.load() && !ctx->wifi_loss_stop.load())) {
            auto stats = monitor->getStats();
            for (auto& [ifindex, s] : stats) {
                // 驱动计数分母 = 成功发包 + 丢包（txDrops 不含在 txPkts 内）
                auto rate = normalizers[ifindex].update(
                    s.txPkts + s.txDrops, s.txDrops);
                if (!rate.has_value()) continue;  // 首轮基线 / 计数器回退 / 无活动
                double txLoss = rate->rate_percent;
                if (txLoss > 0.1) {
                    LOG_INFO(LogModule::NETWORK, "Wi-Fi loss tick: ifindex=" << ifindex
                        << " txLoss=" << txLoss << "%"
                        << " Δdrops=" << rate->delta_drops << "/" << rate->delta_packets);
                }

                char ifname[IF_NAMESIZE] = {0};
                if (if_indextoname(ifindex, ifname) != nullptr && ifname[0] != '\0') {
                    if (ctx->metrics_registry) {
                        ctx->metrics_registry->publish(
                            ifname,
                            weaknet::MetricId::WIFI_LOSS_RATE,
                            weaknet::MetricSample::valid(txLoss)
                        );
                    }
                }
            }
            for (int i = 0; i < static_cast<int>(ctx->cfg.wifi_loss.interval_ms.load() / 100) && (ctx->running.load() && !ctx->wifi_loss_stop.load()); ++i)
                std::this_thread::sleep_for(100ms);
        }
        monitor->stop();
        LOG_INFO(LogModule::NETWORK, "Wi-Fi loss monitor thread stopped");
    });
}

void start_http_latency_monitor_thread(ServerContext* ctx, std::thread* worker, HttpLatencyMonitor* monitor) {
    *worker = std::thread([ctx, monitor]() {
        // The plugin owns this monitor; the worker borrows it until join.
        if (!monitor) return;
        LOG_INFO(LogModule::NETWORK, "HTTP latency monitor thread started");
        while ((ctx->running.load() && !ctx->http_latency_stop.load())) {
            auto globalStats = monitor->getGlobalStats();
            if (globalStats.totalTxns > 0) {
                LOG_INFO(LogModule::NETWORK, "HTTP tick: txns=" << globalStats.totalTxns
                    << " p50=" << (globalStats.p50Ns / 1000000) << "ms"
                    << " p99=" << (globalStats.p99Ns / 1000000) << "ms"
                    << " analysis=" << globalStats.analysis);
            }
            for (int i = 0; i < static_cast<int>(ctx->cfg.http_latency.interval_ms.load() / 100) && (ctx->running.load() && !ctx->http_latency_stop.load()); ++i)
                std::this_thread::sleep_for(100ms);
        }
        monitor->stop();
        LOG_INFO(LogModule::NETWORK, "HTTP latency monitor thread stopped");
    });
}

void start_process_net_profiler_thread(ServerContext* ctx, std::thread* worker, ProcessNetProfiler* profiler) {
    *worker = std::thread([ctx, profiler]() {
        // The plugin owns this monitor; the worker borrows it until join.
        if (!profiler) return;
        LOG_INFO(LogModule::NETWORK, "Process net profiler thread started");
        while ((ctx->running.load() && !ctx->process_profiler_stop.load())) {
            auto topBw = profiler->getTopBandwidth(5);
            for (auto& p : topBw) {
                if (p.txBytes > 0) {
                    LOG_INFO(LogModule::NETWORK, "PROC_BW pid=" << p.pid
                        << " comm=" << p.comm
                        << " txBytes=" << p.txBytes
                        << " retrans=" << p.retransCount);
                }
            }
            auto topRetrans = profiler->getTopRetransmit(5);
            for (auto& p : topRetrans) {
                if (p.retransCount > 0) {
                    LOG_INFO(LogModule::NETWORK, "PROC_RETRANS pid=" << p.pid
                        << " comm=" << p.comm
                        << " retrans=" << p.retransCount
                        << " txBytes=" << p.txBytes);
                }
            }
            for (int i = 0; i < static_cast<int>(ctx->cfg.process_profiler.interval_ms.load() / 100) && (ctx->running.load() && !ctx->process_profiler_stop.load()); ++i)
                std::this_thread::sleep_for(100ms);
        }
        profiler->stop();
        LOG_INFO(LogModule::NETWORK, "Process net profiler thread stopped");
    });
}

void start_tcp_retrans_monitor_thread(ServerContext* ctx, std::thread* worker, TcpRetransMonitor* monitor) {
    *worker = std::thread([ctx, monitor]() {
        // The plugin owns this monitor; the worker borrows it until join.
        if (!monitor) return;
        LOG_INFO(LogModule::NETWORK, "TCP retransmit eBPF monitor thread started");
        while ((ctx->running.load() && !ctx->tcp_retrans_stop.load())) {
            const auto stats = monitor->getStats();
            if (!stats.empty()) {
                LOG_INFO(LogModule::NETWORK, "TCP retransmit tick: connections=" << stats.size()
                    << " lossRate=" << monitor->computeLossRate() << "%");
            }
            for (int i = 0; i < static_cast<int>(ctx->cfg.tcp_retrans.interval_ms.load() / 100) && (ctx->running.load() && !ctx->tcp_retrans_stop.load()); ++i)
                std::this_thread::sleep_for(100ms);
        }
        monitor->stop();
    LOG_INFO(LogModule::NETWORK, "TCP retransmit eBPF monitor thread stopped");
    });
}

void start_tcp_conn_monitor_thread(ServerContext* ctx, std::thread* worker, TcpConnMonitor* monitor) {
    *worker = std::thread([ctx, monitor]() {
        // The plugin owns this monitor; the worker borrows it until join.
        if (!monitor) return;
        LOG_INFO(LogModule::TCP_LOSS, "TCP conn monitor thread started");
        while ((ctx->running.load() && !ctx->tcp_conn_stop.load())) {
            const auto stats = monitor->getStats();
            if (stats.totalAccepts > 0 || stats.totalAcceptFailures > 0) {
                LOG_INFO(LogModule::TCP_LOSS, "TCP conn tick: accepts=" << stats.totalAccepts
                    << " closes=" << stats.totalCloses
                    << " active=" << stats.activeInbound
                    << " acceptFailures=" << stats.totalAcceptFailures
                    << " avgDur=" << stats.avgDurationMs << "ms");
            }
            for (int i = 0; i < static_cast<int>(ctx->cfg.tcp_conn.interval_ms.load() / 100) && (ctx->running.load() && !ctx->tcp_conn_stop.load()); ++i)
                std::this_thread::sleep_for(100ms);
        }
        monitor->stop();
        LOG_INFO(LogModule::TCP_LOSS, "TCP conn monitor thread stopped");
    });
}

// 启动历史数据持久化线程
void start_tcp_connect_monitor_thread(ServerContext* ctx, std::thread* worker, TcpConnectMonitor* monitor) {
    *worker = std::thread([ctx, monitor]() {
        if (!monitor) return;
        LOG_INFO(LogModule::NETWORK, "TCP connect monitor thread started");
        // 与 DNS 捕获同构：高频排空 perf buffer，避免突发建连丢事件
        while ((ctx->running.load() && !ctx->tcp_connect_stop.load())) {
            monitor->drain();
            std::this_thread::sleep_for(200ms);
        }
        monitor->stop();
        LOG_INFO(LogModule::NETWORK, "TCP connect monitor thread stopped");
    });
}

/**
 * @brief 受控主动连通性探测线程
 *
 * 只在 INTERNET_ACCESS Profile 且配置启用时运行。
 * 与被动观测不同：**不能因为最近被动流量充足就跳过**，否则
 * host-level capability 证据会消失。
 */
void start_active_probe_thread(ServerContext* ctx, std::thread* worker) {
    *worker = std::thread([ctx]() {
        LOG_INFO(LogModule::NETWORK, "Active connectivity probe thread started");
        while (ctx->running.load() && !ctx->active_probe_stop.load()) {
            if (ctx->active_probe && ctx->active_probe->isUsable()) {
                ctx->active_probe->runProbeRound();
            }
            // 直接按毫秒计算。此前误把毫秒值当作秒（interval * 10 次 100ms），
            // 导致探测实际约 2.8 小时才跑一轮，结论长期停留在首轮快照。
            const uint32_t interval_ms = std::max(5000u, ctx->cfg.active_probe.interval_ms.load());
            const int ticks = static_cast<int>(interval_ms / 100);
            for (int i = 0; i < ticks &&
                            ctx->running.load() && !ctx->active_probe_stop.load(); ++i) {
                std::this_thread::sleep_for(100ms);
            }
        }
        LOG_INFO(LogModule::NETWORK, "Active connectivity probe thread stopped");
    });
}

void start_history_persistence_thread(ServerContext* ctx) {
    ctx->history_thread = std::thread([ctx](){
        LOG_INFO(LogModule::SYSTEM, "History persistence thread started");

        while (ctx->running.load()) {
            // 每 5 秒持久化一轮（每秒检查 running 标志以便快速退出）
            for (int i = 0; i < 5 && ctx->running.load(); ++i) {
                std::this_thread::sleep_for(1s);
            }
            if (!ctx->running.load()) break;

            if (!ctx->db_mgr || !ctx->db_mgr->isOpen()) continue;

            // 获取当前接口快照并计算质量评分 (CR-3, HR-9)
            auto snapshot = ctx->weak_mgr->getCurrentInterfaces();
            // 无上行时不编造网卡名：写进行里的 iface 必须是真的在用的那张。
            // 真正决定写入哪一行的是下面 isCurrent() + usingNow() 判定，
            // 这里的 active_iface 只服务于"无有效快照"时的占位说明。
            std::string active_iface;
            auto opt = ctx->weak_mgr->getCurrentUsingInterface();
            if (opt.has_value()) active_iface = opt.value();

            // W2 单一事实源：history persistence **只读权威快照**，绝不重新 evaluate。
            // 此前本线程现场只拉 5 个 Core SLE 调 OverallPolicy（无 DNS/TCP/HTTP/Active），
            // 与 quality 线程、HealthCheck 构成三条结论可能不一致的评估路径 ——
            // DB 中写入的是"只有 5 个 SLE 的旧结论"（既有 bug，本处修复）。
            // 原始 metrics 与 assessment **同代冻结**：同一 snapshot 的 experience
            // 提供结论，registry 的 latest 值仅作快照内的指标补充且由 snapshot 时刻界定。
            weaknet::NetworkExperience exp;
            auto snap = ctx->assessment_store.latest();
            const bool snap_valid = snap && weaknet::AssessmentSnapshotStore::isCurrent(*snap,
                    ctx->cfg.config_generation.load(),
                    ctx->dns_tracker ? ctx->dns_tracker->currentBindingEpoch()
                                     : ctx->dns_binding_epoch.load());
            if (snap_valid) {
                exp = snap->experience;
            } else {
                exp.iface = active_iface;
                exp.overall = weaknet::HealthState::UNKNOWN;
                exp.display_score = 50;
                exp.primary_issue = snap ? "stale_assessment" : "no_assessment_yet";
                // 无有效快照时 exp 是默认构造的，其 assessment_profile 会回落
                // 到结构体默认值 INTERNET_ACCESS —— 那是一个编造的 profile。
                // 审计元数据必须如实反映设备实际配置，故这里显式取权威值。
                exp.assessment_profile = currentAssessmentProfile(*ctx);
            }

            double cur_jitter = 0.0;
            if (ctx->metrics_registry) {
                auto j_s = ctx->metrics_registry->latest(active_iface, weaknet::MetricId::JITTER_MS);
                if (j_s.has_value() && j_s->state == weaknet::MetricState::VALID) cur_jitter = j_s->value;
            }

            NetworkQualityResult qualityResult = weaknet::LegacyAdapter::toQualityResult(exp, -1, 0.0, -1000, cur_jitter);

            int written = 0;
            for (const auto& iface : snapshot) {
                if (iface.usingNow()) {
                    if (ctx->db_mgr->insertSnapshot(iface.ifName(), iface, qualityResult,
                                                               iface.generation(), iface.lastUpdatedMs(),
                                                               iface.rttSampleTsMs(), iface.rssiSampleTsMs(),
                                                               iface.jitterSampleTsMs(), iface.tcpLossSampleTsMs(),
                                                               iface.trafficSampleTsMs(),
                                                               weaknet::assessmentProfileToString(
                                                                   exp.assessment_profile))) {
                        written++;
                    }
                }
            }
            if (written > 0) {
                LOG_INFO(LogModule::SYSTEM, "History persistence: wrote " << written << " records");
            }

            // 持久化蓝牙设备与音频质量快照
            if (ctx->bt_monitor && ctx->bt_monitor->isInitialized()) {
                auto adapter = ctx->bt_monitor->getAdapterState();
                auto devices = ctx->bt_monitor->getDevices();
                int bt_written = 0;
                for (const auto& dev : devices) {
                    // 仅记录已连接设备，或信号较强/活跃设备，避免周围瞬态广播垃圾数据占满 DB
                    if (dev.connected || dev.rssiDbm > -75) {
                        BtAudioFusionResult fusion;
                        bool hasAudio = ctx->bt_monitor->getAudioFusionResult(dev.macAddress, &fusion);
                        if (ctx->db_mgr->insertBtSnapshot(
                                adapter.macAddress,
                                dev.macAddress,
                                dev.name.empty() ? dev.alias : dev.name,
                                dev.connected,
                                dev.rssiDbm,
                                dev.estimatedDistance,
                                hasAudio && fusion.isActive,
                                hasAudio ? fusion.qualityScore : 0.0,
                                hasAudio && fusion.suspectedStall,
                                hasAudio ? fusion.bytesPerSec : 0,
                                hasAudio ? fusion.maxGapMs : 0)) {
                            bt_written++;
                        }
                    }
                }
                if (bt_written > 0) {
                    LOG_INFO(LogModule::SYSTEM, "History persistence: wrote " << bt_written << " Bluetooth records");
                }
            }

            // 每天清理一次过期日志文件
            auto now = std::chrono::steady_clock::now();
            static auto last_log_cleanup = std::chrono::steady_clock::now();
            if (now - last_log_cleanup > std::chrono::hours(24)) {
                int log_deleted = Logger::cleanOldLogs("./logs/server", 7);
                last_log_cleanup = now;
                if (log_deleted > 0) {
                    LOG_INFO(LogModule::SYSTEM, "History persistence: cleaned " << log_deleted << " old log files");
                }
            }
        }

        LOG_INFO(LogModule::SYSTEM, "History persistence thread stopped");
    });
}

// 启动服务
int start_server(int argc, char** argv) {
    // 解析命令行参数：--config <path>（默认 /etc/weaknet/config.yaml）
    std::string config_path = "/etc/weaknet/config.yaml";
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--config") {
            config_path = argv[i + 1];
            ++i;
        }
    }
    // 配置失败 → 直接退出，不写日志（init 还没起）
    // 注意：此函数内已有 ctx 之前不能 LOG_*
    std::cerr << "Loading config from: " << config_path << std::endl;

    // 解析日志级别（用于 Logger::init）
    LogLevel log_level = LogLevel::INFO;
    if (!parseLogLevel("info", &log_level)) log_level = LogLevel::INFO;

    // 初始化日志系统
    if (!Logger::init("server", "./logs/server", log_level, true)) {
        std::cerr << "Failed to initialize logger" << std::endl;
        return 1;
    }

    // 启动带时间戳的文件日志
    Logger::startFileLog("./server/log");

    // 全局网络与通信库初始化（单线程早期调用）
    weaknet::EdgeTelemetryExporter::initGlobal();

    // 启动时清理 7 天前的日志文件
    int cleaned = Logger::cleanOldLogs("./logs/server", 7);
    if (cleaned > 0) {
        LOG_INFO(LogModule::SYSTEM, "Cleaned " << cleaned << " old log files on startup");
    }

    // 注册信号处理函数（SIGINT/SIGTERM）
    std::signal(SIGINT, Logger::signalHandler);
    std::signal(SIGTERM, Logger::signalHandler);

    ServerContext ctx;

    // 加载配置文件（文件不存在 → 默认值；存在但语法错 → exit 1）
    {
        std::string cfg_err;
        if (!loadWeakNetConfig(config_path, &ctx.cfg, &cfg_err)) {
            std::cerr << "Configuration error in " << config_path << ": " << cfg_err << std::endl;
            LOG_ERROR(LogModule::SYSTEM, "Configuration error in " << config_path << ": " << cfg_err);
            return 2;
        }
        LOG_INFO(LogModule::SYSTEM, "Config loaded from: " << config_path
            << " (dbus name: " << kBusName << ")");

        // 应用日志级别（配置文件覆盖默认）
        LogLevel new_level;
        if (parseLogLevel(ctx.cfg.log_level.get(), &new_level)) {
            Logger::setLogLevel(new_level);
            LOG_INFO(LogModule::SYSTEM, "Log level from config: " << ctx.cfg.log_level.get());
        } else if (!ctx.cfg.log_level.get().empty()) {
            LOG_WARNING(LogModule::SYSTEM,
                "Unknown log_level '" << ctx.cfg.log_level.get() << "', keeping default INFO");
        }

        // IR-3: assessment profile 合法性校验（权威值始终是 cfg.dns.assessment_profile，
        // 不再拷贝到独立成员——拷贝会让运行时 SetMonitorParam 静默失效）。
        // 非法值仅告警并按 INTERNET_ACCESS 回落，由 parseAssessmentProfile 统一实现。
        {
            const std::string p = ctx.cfg.dns.assessment_profile.get();
            if (p == "NETWORK_ONLY" || p == "INTERNET_ACCESS") {
                LOG_INFO(LogModule::SYSTEM, "Assessment profile from config: " << p);
            } else {
                LOG_WARNING(LogModule::SYSTEM, "Unknown assessment_profile '" << p
                            << "', falling back to INTERNET_ACCESS");
            }
        }
    }

    if (!init_dbus(&ctx)) return 1;

    // 启动事件监控
    getEventManager().startEventMonitoring(&ctx);

    // 初始化 MetricsRegistry 与 WeakNetMgr (MR-1, MR-2, HR-4)
    ctx.metrics_registry = std::make_unique<weaknet::MetricsRegistry>();
    ctx.dns_tracker = std::make_unique<weaknet::DnsTransactionTracker>();
    ctx.weak_mgr = std::make_unique<WeakNetMgr>(ctx.metrics_registry.get());

    // 网络代次跨重启推进（W-edge 上行去重的正确性前提）。
    uint64_t persistent_epoch = 1;
    {
        weaknet::NetworkEpochStore epoch_store(resolveNetworkEpochPath(ctx.cfg.data_dir.get()));
        persistent_epoch = epoch_store.open();
        ctx.dns_binding_epoch.store(persistent_epoch);
        // dns_tracker 的 binding epoch 初值为 1，运行中由路由/解析器变化推进。
        // 持久化值只在更大时才生效（advanceBindingEpoch 内部拒绝回退），
        // 因此这里把两者对齐到同一个下界。
        ctx.dns_tracker->advanceBindingEpoch(persistent_epoch);
    }

    // 初始化端侧确定性诊断引擎与证据生成服务
    {
        std::string dev_id = ctx.cfg.edge.device_id.get();
        if (dev_id.empty()) {
            dev_id = "radxa-cubie-a7a";
        }
        ctx.action_registry = std::make_shared<weaknet::ActionRegistry>();
        ctx.diagnosis_engine = std::make_unique<weaknet::DiagnosisEngine>(
            weaknet::RuleLoader::loadDefaultRules(), ctx.action_registry);
        ctx.evidence_id_generator = std::make_unique<weaknet::EvidenceIdGenerator>(
            dev_id, persistent_epoch, true);
        LOG_INFO(LogModule::SYSTEM, "DiagnosisEngine & ActionRegistry initialized for device: " << dev_id);
    }

    // 启动 UsingInterfaceManager（一次性启动，不重复调用 start()）
    UsingInterfaceManager::getInstance()->start();

    // 初始化历史数据持久化管理器（智能指针）
    const std::string db_path = resolveDatabasePath(ctx.cfg.data_dir.get());
    LOG_INFO(LogModule::SYSTEM, "initializing database manager (path=" << db_path << ")");
    ctx.db_mgr = std::make_unique<DatabaseManager>(db_path);
    if (ctx.db_mgr->isOpen()) {
        LOG_INFO(LogModule::SYSTEM, "database manager opened, records=" << ctx.db_mgr->getRecordCount());
    } else {
        LOG_WARNING(LogModule::SYSTEM, "database manager failed to open, history persistence disabled");
    }

    // ================================================================
    // 受控主动连通性探测：宿主 Internet 能力证据的唯一来源
    // 默认关闭；targets 为空时不启用（不内置任何第三方默认目标，
    // 避免第三方服务异常被误读为 Internet 故障）。
    // ================================================================
    {
        ctx.active_probe = std::make_unique<ActiveConnectivityMonitor>();
        ActiveProbeConfig ap;
        ap.enabled = ctx.cfg.active_probe.enabled.load();
        const uint32_t interval_ms = std::max(5000u, ctx.cfg.active_probe.interval_ms.load());
        const uint32_t timeout_ms = std::max(500u, ctx.cfg.active_probe.timeout_ms.load());
        ap.interval_sec = interval_ms / 1000u;
        ap.timeout_sec = std::max(1u, timeout_ms / 1000u);

        // 解析 targets："id|hostname|port|domain|path|expect_body"
        // 后三段可选：
        //   domain      —— 故障域标识（缺省取 hostname）
        //   path        —— 该目标的 HTTP 路径（仅 Portal oracle 用）
        //   expect_body —— 该目标的预期正文（仅 Portal oracle 用）
        // 真实 connectivity-check 端点的路径与预期正文互不相同，
        // 必须能按目标声明（见 ActiveProbeTargetConfig 的说明）。
        //
        // 必须**手动按 '|' 切分**而不是反复 std::getline：getline 在读到
        // 末尾空字段（"x|" 的最后一个字段）时提取 0 字符 → 置 failbit →
        // 返回 false，于是"显式留空"与"字段不存在"无法区分。
        // 真机实测后果：generate_204 的 `...|/generate_204|` 被判成"未声明
        // 预期"，回落全局文本预期 → 空正文永远 CONTENT_MISMATCH。
        const auto parseTargets = [](const std::string& raw) {
            std::vector<ActiveProbeTargetConfig> out;
            size_t pos = 0;
            while (pos <= raw.size()) {
                const size_t comma = raw.find(',', pos);
                const std::string item = raw.substr(
                    pos, comma == std::string::npos ? std::string::npos : comma - pos);
                pos = (comma == std::string::npos) ? raw.size() + 1 : comma + 1;
                if (item.empty()) continue;

                // 按 '|' 切分，保留末尾空字段
                std::vector<std::string> f;
                size_t fp = 0;
                for (;;) {
                    const size_t bar = item.find('|', fp);
                    if (bar == std::string::npos) { f.push_back(item.substr(fp)); break; }
                    f.push_back(item.substr(fp, bar - fp));
                    fp = bar + 1;
                }

                ActiveProbeTargetConfig t;
                t.id = f.size() > 0 ? f[0] : "";
                t.hostname = f.size() > 1 ? f[1] : "";
                const std::string port = f.size() > 2 ? f[2] : "";
                const std::string domain = f.size() > 3 ? f[3] : "";
                t.tcp_port = port.empty() ? 443 : static_cast<uint16_t>(std::atoi(port.c_str()));
                t.failure_domain = domain.empty() ? t.hostname : domain;
                t.http_path = f.size() > 4 ? f[4] : "";
                if (f.size() > 5) {
                    t.expect_body = f[5];
                    // 字段存在即为"已声明"，即使为空串（generate_204 场景）
                    t.expect_body_specified = true;
                }
                if (t.hostname.empty()) continue;
                out.push_back(t);
            }
            return out;
        };

        ap.targets = parseTargets(ctx.cfg.active_probe.targets.get());

        // HTTPS capability：编译期无 TLS 依赖时如实降级，不伪装
        ap.https_enabled = ctx.cfg.active_probe.https_enabled.load();
        if (ap.https_enabled && !ActiveConnectivityMonitor::tlsAvailable()) {
            LOG_WARNING(LogModule::NETWORK,
                "active_probe.https_enabled=true but this build has no TLS "
                "(WEAKNET_HAVE_TLS undefined); HTTPS capability will report "
                "NO_CAPABILITY (no_tls_probe_capability).");
        }

        // Captive Portal 受控 oracle
        ap.portal.enabled = ctx.cfg.active_probe.portal_check_enabled.load();
        ap.portal.path = ctx.cfg.active_probe.portal_path.get();
        ap.portal.expect_body = ctx.cfg.active_probe.portal_expect_body.get();
        ap.portal.targets = parseTargets(ctx.cfg.active_probe.portal_targets.get());
        if (ap.portal.enabled) {
            if (ap.portal.targets.empty()) {
                LOG_WARNING(LogModule::NETWORK,
                    "active_probe.portal_check_enabled=true but portal_targets is empty; "
                    "Portal will report NO_CAPABILITY. Portal verdict requires "
                    "controlled oracle endpoints with known responses.");
                ap.portal.enabled = false;
            } else if (ap.portal.expect_body.empty()) {
                LOG_WARNING(LogModule::NETWORK,
                    "Portal oracle configured without portal_expect_body; only "
                    "redirect-away signals will be considered (content "
                    "fingerprint comparison disabled).");
            }
        }

        ctx.active_probe->configure(ap);
        LOG_INFO(LogModule::NETWORK,
                 "Active probe detail: capability_targets=" << ap.targets.size()
                 << " https_enabled=" << (ap.https_enabled ? "true" : "false")
                 << " tls_available=" << (ActiveConnectivityMonitor::tlsAvailable() ? "true" : "false")
                 << " portal_enabled=" << (ap.portal.enabled ? "true" : "false")
                 << " portal_targets=" << ap.portal.targets.size()
                 << " portal_path=" << ap.portal.path
                 << " portal_expect_body=" << (ap.portal.expect_body.empty() ? "(empty)" : ap.portal.expect_body));
        for (const auto& t : ap.portal.targets) {
            LOG_INFO(LogModule::NETWORK, "Portal oracle target: id=" << t.id
                     << " host=" << t.hostname
                     << " port=" << t.tcp_port
                     << " domain=" << t.failure_domain
                     << " path=" << (t.http_path.empty() ? ap.portal.path : t.http_path)
                     << " expect_body=" << (t.expect_body.empty() ? ap.portal.expect_body : t.expect_body));
        }

        if (ap.enabled && ap.targets.size() < 2) {
            LOG_WARNING(LogModule::NETWORK,
                "Active probe enabled but fewer than 2 targets configured; "
                "capability verdict requires >=2 independent targets. "
                "Active capability will report UNKNOWN.");
        }
        LOG_INFO(LogModule::NETWORK, "Active connectivity probe: "
                 << ctx.active_probe->describeConfig());
        start_active_probe_thread(&ctx, &ctx.active_probe_thread);
    }

    // 初始化接口列表到WeakNetMgr中
    LOG_INFO(LogModule::WEAK_MGR, "initializing interface list...");
    auto initial_interfaces = ctx.weak_mgr->collectCurrentInterfaces();
    ctx.weak_mgr->updateInterfaces(initial_interfaces);
    LOG_INFO(LogModule::WEAK_MGR, "interface list initialized with " << initial_interfaces.size() << " interfaces");

    // ================================================================
    // 监控器插件化启动：注册内置插件 → 实例化（按 order 排序）→ init → start
    // ================================================================
    registerBuiltinPlugins();
    registerEbpfPlugins();
    ctx.monitor_manager = std::make_unique<MonitorManager>(
        &ctx, instantiateAllPlugins());
    const std::string db_path_for_overrides = resolveDatabasePath(ctx.cfg.data_dir.get());
    const std::string base_dir_for_overrides =
        db_path_for_overrides.substr(0, db_path_for_overrides.size() - std::string("history.db").size());
    ctx.monitor_manager->setOverridePath(base_dir_for_overrides + "runtime-overrides.bin");
    // 生命周期状态变化 → D-Bus MonitorStateChanged 信号（携带 "name:state"）。
    ctx.monitor_manager->setStateChangeCallback(
        [&ctx](const std::string& name, const std::string& state) {
            if (ctx.service) {
                ctx.service->emitSpecificSignal(kSignalMonitorStateChanged, name + ":" + state, 0);
            }
        });
    if (!ctx.monitor_manager->loadOverrides(nullptr)) {
        LOG_WARNING(LogModule::SYSTEM, "runtime monitor overrides could not be loaded");
    }
    if (!ctx.monitor_manager->startConfigured()) {
        LOG_WARNING(LogModule::NETWORK,
                    "one or more monitor plugins failed to initialize or start");
    }

    // 启动历史数据持久化线程（非监控器，server.cpp 单独管理）
    if (ctx.db_mgr && ctx.db_mgr->isOpen()) {
        LOG_INFO(LogModule::SYSTEM, "starting history persistence thread (interval=5s)");
        start_history_persistence_thread(&ctx);
    }

    // 启动边缘遥测上报（可选，W-edge）。
    // 配置不完整/能力缺失时 start() 返回 false 并保持关闭，绝不半启用。
    //
    // 先构造配置事务（STABLE/TRIAL/ROLLBACK 状态机），再把它交给 exporter。
    // state_path 落在与 history.db 同一目录，崩溃后可由 hasCrashRecoveryFile
    // 检测到并触发 forceRollback。
    const std::string txn_state_path =
        resolveDatabasePath(ctx.cfg.data_dir.get())
            .substr(0, resolveDatabasePath(ctx.cfg.data_dir.get()).size() -
                            std::string("history.db").size()) +
        "config_txn_state";
    ctx.config_txn = std::make_shared<weaknet_dbus::ConfigTransaction>(txn_state_path);

    ctx.edge_exporter = std::make_unique<weaknet::EdgeTelemetryExporter>(
        ctx.cfg, /*hostname=*/"edge-node", ctx.config_txn);

    // 启动时若发现残留的 prior_values（上次 TRIAL 中崩溃），立即回滚还原。
    if (ctx.config_txn->hasCrashRecoveryFile()) {
        LOG_WARNING(LogModule::SYSTEM,
                    "检测到上次 TRIAL 中的崩溃残留，立即执行回滚还原: " << txn_state_path);
        ctx.config_txn->forceRollback(&ctx.cfg, "crash_recovery");
    }

    if (ctx.edge_exporter->start()) {
        LOG_INFO(LogModule::SYSTEM, "edge telemetry exporter started");
    }

    // 主线程进入阻塞式 looper
    auto* lp = Looper::current();
    lp->attach(ctx.connection);
    lp->run(&ctx);
    // Looper::run() 退出后，按顺序收尾：
    // 1) 先置 running=false 让所有监控线程退出循环
    // 2) MonitorManager 逐插件请求停止并 join worker，确保不再访问 ctx
    // 3) 之后仅 join 服务级历史持久化线程，再释放共享资源
    LOG_INFO(LogModule::NETWORK, "server shutting down, stopping monitor threads...");
    ctx.running = false;

    // 先由每个插件请求停止并 join 自己的 worker；MonitorManager 是唯一插件停止入口。
    // stop() 内部设置 per-monitor flag，避免全局 running=false 影响其他插件。
    if (ctx.monitor_manager) {
        ctx.monitor_manager->stopAll();
    }

    // 服务级历史线程与受控主动探测线程由 ServerContext 统一 join；插件线程已由各自 stop() 完成。
    if (ctx.active_probe_thread.joinable())             ctx.active_probe_thread.join();

    // 历史持久化线程：只读 weak_mgr 快照 + 写 DB，不依赖其他线程资源，最后 join 最安全。
    // 此前缺失该 join，导致 ~ServerContext 析构时该线程可能仍持 ctx* 访问 → 悬垂/terminate。
    if (ctx.history_thread.joinable())                  ctx.history_thread.join();

    // 停止边缘遥测上报：先于 ~ServerContext，保证不再访问 ctx.cfg。
    if (ctx.edge_exporter) {
        ctx.edge_exporter->stop();
        ctx.edge_exporter.reset();
    }

    // 停止文件日志（在 glog 关闭之前）
    Logger::stopFileLog();

    // 清理glog。资源（DBus 连接 / service / weak_mgr）由 ~ServerContext 统一释放。
    google::ShutdownGoogleLogging();

    // 清理全局网络通信库（libcurl 全局资源释放）
    weaknet::EdgeTelemetryExporter::cleanupGlobal();

    return 0;
}

}  // namespace weaknet_dbus



