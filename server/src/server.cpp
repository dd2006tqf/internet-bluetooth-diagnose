// server.cpp
// 使用 libdbus-1 提供服务端：导出 Get 方法与 Changed 信号；并提供 start_server 作为入口

#include <dbus/dbus.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
#include <algorithm>
#include <mutex>
#include <csignal>

#include "common.hpp"
#include "serializer.hpp"
#include "net_iface.h"
#include "server.hpp"
#include "dbus_service.hpp"
#include "looper.hpp"
#include "net_info.hpp"
#include "weak_netmgr.hpp"
#include "rtt_monitor.hpp"
#include "rssi_monitor.hpp"
#include "tcp_loss_monitor.hpp"
#include "jitter_monitor.hpp"
#include "event_manager.hpp"
#include "logger.hpp"
#include "network_quality_assessor.hpp"
#include "bt_monitor.hpp"
#include "band_conflict_detector.hpp"
#include "bt_audio_fusion.hpp"
#include "dns_monitor.hpp"
#include "wifi_packet_loss_monitor.hpp"
#include "http_latency_monitor.hpp"
#include "process_net_profiler.hpp"
#include <iomanip>

using namespace std::chrono_literals;

namespace weaknet_dbus {

// ServerContext 析构：释放 DBus 连接与 service/weak_mgr 资源。
// 调用前提：所有捕获 ctx* 的监控线程已在 start_server() 退出路径完成 join，
// 之后才进入本析构，避免在回收线程访问尚未析构的成员。
ServerContext::~ServerContext() {
    if (connection) {
        dbus_connection_close(connection);
        dbus_connection_unref(connection);
        connection = nullptr;
    }
    if (service) { delete service; service = nullptr; }
    if (weak_mgr) { delete weak_mgr; weak_mgr = nullptr; }
}

// 共享列表迁移至 ServerContext，在 server.hpp 中定义

// 使用 DbusService 替代手写处理函数

// 将字符串作为方法返回，通过序列化保存到文件
// Get 方法处理已迁移到 DbusService

// 发送 Changed 信号，并将载荷序列化到文件
// 发信号也迁移到 DbusService

// 处理进入总线的消息（方法调用等）
// 消息处理迁移，由 DbusService::MessageHandler 提供

// 将字符串数组作为返回
static bool replyStringArray(DBusConnection* conn, DBusMessage* msg, const std::vector<std::string>& arr) {
    DBusMessage* reply = dbus_message_new_method_return(msg);
    if (!reply) return false;

    DBusMessageIter iter;
    dbus_message_iter_init_append(reply, &iter);

    DBusMessageIter array_iter;
    int element_type = DBUS_TYPE_STRING;
    if (!dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, DBUS_TYPE_STRING_AS_STRING, &array_iter)) {
        dbus_message_unref(reply);
        return false;
    }

    for (const auto& s : arr) {
        const char* cs = s.c_str();
        if (!dbus_message_iter_append_basic(&array_iter, element_type, &cs)) {
            dbus_message_iter_close_container(&iter, &array_iter);
            dbus_message_unref(reply);
            return false;
        }
    }

    if (!dbus_message_iter_close_container(&iter, &array_iter)) {
        dbus_message_unref(reply);
        return false;
    }

    bool ok = dbus_connection_send(conn, reply, nullptr);
    dbus_connection_flush(conn);
    dbus_message_unref(reply);
    return ok;
}

// 字符串数组回复已封装到 DbusService

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
    LOG_INFO(LogModule::DBUS, "init_dbus: start connecting to session bus...");
    dbus_threads_init_default();

    DBusError err;
    dbus_error_init(&err);

    DBusConnection* conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (dbus_error_is_set(&err)) {
        LOG_ERROR(LogModule::DBUS, "连接总线失败: " << err.message);
        dbus_error_free(&err);
    }
    if (!conn) return nullptr;
    LOG_INFO(LogModule::DBUS, "connected to session bus");

    LOG_INFO(LogModule::DBUS, "requesting bus name: " << kBusName);
    int ret = dbus_bus_request_name(conn, kBusName, DBUS_NAME_FLAG_REPLACE_EXISTING, &err);
    if (dbus_error_is_set(&err)) {
        LOG_ERROR(LogModule::DBUS, "请求服务名失败: " << err.message);
        dbus_error_free(&err);
    }
    if (ret != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
        LOG_ERROR(LogModule::DBUS, "未能成为主拥有者，ret=" << ret);
        return nullptr;
    }

    // 使用服务类进行对象注册（保存到上下文，统一管理生命周期）
    LOG_INFO(LogModule::DBUS, "registering object path: " << kObjectPath << " (interface=" << kInterface << ")");
    ctx->service = new DbusService(ctx);
    if (!ctx->service->register_on_connection(conn)) {
        LOG_ERROR(LogModule::DBUS, "注册对象路径失败");
        delete ctx->service;
        ctx->service = nullptr;
        return nullptr;
    }
    // 指针已保存至 ctx

    ctx->connection = conn;
    LOG_INFO(LogModule::DBUS, "DBus 服务端已启动，接口 " << kInterface << "，方法 " << kMethodGet << "，信号 " << kSignalChanged);
    return conn;
}

// 独立接口：启动网卡监控线程（使用 WeakNetMgr 与 NetInfo）
void start_iface_monitor_thread(ServerContext* ctx) {
    ctx->iface_thread = std::thread([ctx](){
        LOG_INFO(LogModule::INTERFACE, "monitor thread started");
        std::vector<NetInfo> current;
        int32_t change_counter = 0;

        while (ctx->running.load()) {
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
            for (int i = 0; i < 100 && ctx->running.load(); ++i)
                std::this_thread::sleep_for(100ms);
        }
    });
}

// 独立接口：启动流量分析线程
static void start_traffic_analysis_thread(ServerContext* ctx) {
    ctx->traffic_analysis_thread = std::thread([ctx](){
        LOG_INFO(LogModule::WEAK_MGR, "traffic analysis thread started");

        // 启动流量分析器（使用当前接口；原实现硬编码 wlan0，保持既有行为）
        ctx->weak_mgr->startTrafficAnalysis("wlan0", 10);
        
        int loop_count = 0;
        while (ctx->running.load()) {
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
            
            for (int i = 0; i < 100 && ctx->running.load(); ++i)
                std::this_thread::sleep_for(100ms);
        }
        ctx->weak_mgr->stopTrafficAnalysis();
        LOG_INFO(LogModule::WEAK_MGR, "traffic analysis thread stopped");
    });
}

// 独立接口：启动"当前上网网卡"监控线程（使用 UsingInterfaceManager）
static void start_using_iface_thread(ServerContext* ctx) {
    ctx->using_thread = std::thread([ctx](){
        LOG_INFO(LogModule::WEAK_MGR, "monitor thread started");

        int loop_count = 0;
        while (ctx->running.load()) {
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
            for (int i = 0; i < 100 && ctx->running.load(); ++i)
                std::this_thread::sleep_for(100ms);
        }
    });
}

// 独立接口：启动网络质量监控线程
static void start_network_quality_thread(ServerContext* ctx) {
    ctx->network_quality_thread = std::thread([ctx](){
        LOG_INFO(LogModule::WEAK_MGR, "network quality monitor thread started");
        
        NetworkQualityAssessor assessor;
        NetworkQualityResult lastQuality;
        lastQuality.level = NetworkQualityLevel::UNKNOWN;
        
        int loop_count = 0;
        while (ctx->running.load()) {
            loop_count++;
            LOG_INFO(LogModule::WEAK_MGR, "network quality thread running, loop=" << loop_count);
            try {
                // 直接获取当前接口列表（线程安全）
                auto currentInterfaces = ctx->weak_mgr->getCurrentInterfaces();
                LOG_INFO(LogModule::WEAK_MGR, "network quality: current interfaces count=" << currentInterfaces.size());
                
                // 评估网络质量
                NetworkQualityResult currentQuality = assessor.assessQuality(currentInterfaces);
                
                // 检查质量是否发生变化
                if (currentQuality.level != lastQuality.level || 
                    std::abs(currentQuality.score - lastQuality.score) > 15.0) {
                    
                    LOG_INFO(LogModule::WEAK_MGR, "网络质量变化: " << currentQuality.levelName 
                        << " (分数: " << std::fixed << std::setprecision(1) << currentQuality.score << ")");
                    
                    // 发送网络质量变化事件
                    getEventManager().emitNetworkQualityChanged(
                        currentQuality.levelName, 
                        currentQuality.details, 
                        "network_quality_assessor"
                    );
                    
                    lastQuality = currentQuality;
                } else {
                    LOG_INFO(LogModule::WEAK_MGR, "网络质量稳定: " << currentQuality.levelName 
                        << " (分数: " << std::fixed << std::setprecision(1) << currentQuality.score << ")");
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

                // 获取当前上网接口的 Wi-Fi RSSI
                int wifiRssi = -1000;
                {
                    auto interfaces = ctx->weak_mgr->getCurrentInterfaces();
                    for (const auto& iface : interfaces) {
                        if (iface.usingNow() && iface.hasRssi()) {
                            wifiRssi = iface.rssiDbm();
                            break;
                        }
                    }
                    // 若无 usingNow 接口，退而取第一个有 RSSI 的 Wi-Fi 接口
                    if (wifiRssi <= -1000) {
                        for (const auto& iface : interfaces) {
                            if (iface.hasRssi()) {
                                wifiRssi = iface.rssiDbm();
                                break;
                            }
                        }
                    }
                }

                // 获取蓝牙 RSSI（取所有已连接设备的平均 RSSI）
                int btRssi = -1000;
                if (auto* mon = ctx->bt_monitor.load(); mon && mon->isInitialized()) {
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

                auto conflictResult = conflictDetector.detect();
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
                BtMonitor* mon = ctx->bt_monitor.load();
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
            
            for (int i = 0; i < 150 && ctx->running.load(); ++i)
                std::this_thread::sleep_for(100ms);
        }
        
        LOG_INFO(LogModule::WEAK_MGR, "network quality monitor thread stopped");
    });
}

// ====================================================================
// eBPF 监控器线程启动函数
// 将此前孤立的 BPF 监控器纳入 ServerContext 统一生命周期
// ====================================================================

void start_dns_monitor_thread(ServerContext* ctx) {
    // 指针成员改为 atomic，读写用 load()/store()（TSan 证实 dbus_service 读 vs
    // 此处写是数据竞争根因）。捕获 ctx 的副本并判空属额外防御。
    ServerContext* const ctx_capture = ctx;
    ctx_capture->dns_monitor_thread = std::thread([ctx_capture]() {
        if (!ctx_capture) return;
        LOG_INFO(LogModule::NETWORK, "DNS monitor thread started");
        auto monitor = std::make_unique<DnsMonitor>();
        ctx_capture->dns_monitor.store(monitor.get());
        if (!monitor->init("build/dns_monitor.bpf.o")) {
            LOG_INFO(LogModule::NETWORK, "DNS monitor: BPF init failed, thread exiting");
            ctx_capture->dns_monitor.store(nullptr);
            return;
        }
        while (ctx_capture->running.load()) {
            auto stats = monitor->getStats();
            if (stats.totalQueries > 0) {
                LOG_INFO(LogModule::NETWORK, "DNS tick: queries=" << stats.totalQueries
                    << " avgLatency=" << stats.avgLatencyMs << "ms"
                    << " timeoutRate=" << stats.timeoutRate() << "%");
            }
            for (int i = 0; i < 100 && ctx_capture->running.load(); ++i)
                std::this_thread::sleep_for(100ms);
        }
        monitor->stop();
        ctx_capture->dns_monitor.store(nullptr);
        LOG_INFO(LogModule::NETWORK, "DNS monitor thread stopped");
    });
}

void start_wifi_loss_monitor_thread(ServerContext* ctx) {
    ctx->wifi_loss_monitor_thread = std::thread([ctx]() {
        LOG_INFO(LogModule::NETWORK, "Wi-Fi loss monitor thread started");
        auto monitor = std::make_unique<WifiPacketLossMonitor>();
        ctx->wifi_loss_monitor.store(monitor.get());
        if (!monitor->init("build/wifi_packet_loss.bpf.o")) {
            LOG_INFO(LogModule::NETWORK, "Wi-Fi loss monitor: BPF init failed, thread exiting");
            ctx->wifi_loss_monitor.store(nullptr);
            return;
        }
        while (ctx->running.load()) {
            auto stats = monitor->getStats();
            for (auto& [ifindex, s] : stats) {
                double txLoss = s.txLossRate();
                if (txLoss > 0.1) {
                    LOG_INFO(LogModule::NETWORK, "Wi-Fi loss tick: ifindex=" << ifindex
                        << " txLoss=" << txLoss << "%"
                        << " txDrops=" << s.txDrops << "/" << s.txPkts);
                }
            }
            for (int i = 0; i < 100 && ctx->running.load(); ++i)
                std::this_thread::sleep_for(100ms);
        }
        monitor->stop();
        ctx->wifi_loss_monitor.store(nullptr);
        LOG_INFO(LogModule::NETWORK, "Wi-Fi loss monitor thread stopped");
    });
}

void start_http_latency_monitor_thread(ServerContext* ctx) {
    ctx->http_latency_monitor_thread = std::thread([ctx]() {
        LOG_INFO(LogModule::NETWORK, "HTTP latency monitor thread started");
        auto monitor = std::make_unique<HttpLatencyMonitor>();
        ctx->http_latency_monitor.store(monitor.get());
        if (!monitor->init("build/http_latency.bpf.o")) {
            LOG_INFO(LogModule::NETWORK, "HTTP latency monitor: BPF init failed, thread exiting");
            ctx->http_latency_monitor.store(nullptr);
            return;
        }
        while (ctx->running.load()) {
            auto globalStats = monitor->getGlobalStats();
            if (globalStats.totalTxns > 0) {
                LOG_INFO(LogModule::NETWORK, "HTTP tick: txns=" << globalStats.totalTxns
                    << " p50=" << (globalStats.p50Ns / 1000000) << "ms"
                    << " p99=" << (globalStats.p99Ns / 1000000) << "ms"
                    << " analysis=" << globalStats.analysis);
            }
            for (int i = 0; i < 100 && ctx->running.load(); ++i)
                std::this_thread::sleep_for(100ms);
        }
        monitor->stop();
        ctx->http_latency_monitor.store(nullptr);
        LOG_INFO(LogModule::NETWORK, "HTTP latency monitor thread stopped");
    });
}

void start_process_net_profiler_thread(ServerContext* ctx) {
    ctx->process_net_profiler_thread = std::thread([ctx]() {
        LOG_INFO(LogModule::NETWORK, "Process net profiler thread started");
        auto profiler = std::make_unique<ProcessNetProfiler>();
        ctx->process_net_profiler.store(profiler.get());
        if (!profiler->init("build/flow_rate.bpf.o")) {
            LOG_INFO(LogModule::NETWORK, "Process net profiler: BPF init failed, thread exiting");
            ctx->process_net_profiler.store(nullptr);
            return;
        }
        while (ctx->running.load()) {
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
            for (int i = 0; i < 150 && ctx->running.load(); ++i)
                std::this_thread::sleep_for(100ms);
        }
        profiler->stop();
        ctx->process_net_profiler.store(nullptr);
        LOG_INFO(LogModule::NETWORK, "Process net profiler thread stopped");
    });
}

// 启动服务
int start_server() {
    // 初始化日志系统
    if (!Logger::init("server", "./logs/server", LogLevel::INFO, true)) {
        std::cerr << "Failed to initialize logger" << std::endl;
        return 1;
    }

    // 启动带时间戳的文件日志
    Logger::startFileLog("./server/log");

    // 注册信号处理函数（SIGINT/SIGTERM）
    std::signal(SIGINT, Logger::signalHandler);
    std::signal(SIGTERM, Logger::signalHandler);

    ServerContext ctx;
    if (!init_dbus(&ctx)) return 1;

    // 启动事件监控
    getEventManager().startEventMonitoring(&ctx);

    // 初始化WeakNetMgr（一次性预建，各监控线程不再各自 new）
    if (!ctx.weak_mgr) ctx.weak_mgr = new WeakNetMgr();

    // 初始化接口列表到WeakNetMgr中
    LOG_INFO(LogModule::WEAK_MGR, "initializing interface list...");
    auto initial_interfaces = ctx.weak_mgr->collectCurrentInterfaces();
    ctx.weak_mgr->updateInterfaces(initial_interfaces);
    LOG_INFO(LogModule::WEAK_MGR, "interface list initialized with " << initial_interfaces.size() << " interfaces");

    start_iface_monitor_thread(&ctx);
    start_using_iface_thread(&ctx);
    // 启动 RTT 监控线程：使用阿里云 DNS 223.5.5.5 作为目标
    LOG_INFO(LogModule::RTT, "starting monitor thread (target=223.5.5.5, interval=10s)");
    start_rtt_monitor_thread(&ctx, "223.5.5.5", /*intervalMs*/10000, /*timeoutMs*/800);
    // 启动网络抖动(Jitter)监控线程：基于 RTT 样本标准差评估延迟稳定性
    LOG_INFO(LogModule::NETWORK, "starting jitter monitor thread (target=223.5.5.5, interval=2s, window=30)");
    start_jitter_monitor_thread(&ctx, "223.5.5.5", /*intervalMs*/2000, /*timeoutMs*/800, /*windowSize*/30);
    // 启动 Wi-Fi RSSI 监控线程（wpa_supplicant ctrl 目录自动探测）
    LOG_INFO(LogModule::RSSI, "starting RSSI monitor thread (interval=10s)");
    start_rssi_monitor_thread(&ctx);
    // 启动 TCP 丢包率监控线程
    LOG_INFO(LogModule::TCP_LOSS, "starting TCP loss rate monitor thread (interval=10s)");
    start_tcp_loss_monitor_thread(&ctx);
    // 启动流量分析线程
    LOG_INFO(LogModule::WEAK_MGR, "starting traffic analysis thread (interval=10s)");
    start_traffic_analysis_thread(&ctx);
    // 启动网络质量监控线程
    LOG_INFO(LogModule::WEAK_MGR, "starting network quality monitor thread (interval=15s)");
    start_network_quality_thread(&ctx);
    // 启动蓝牙监测线程 (通过 BlueZ D-Bus API)
    LOG_INFO(LogModule::BLUETOOTH, "starting bluetooth monitor thread");
    // ctx.bt_monitor 是 atomic<BtMonitor*>，不能取地址作 BtMonitor**；改传 nullptr 并在
    // start_bt_monitor_thread 内部已直接写 ctx->bt_monitor.store()（见其实现）。
    start_bt_monitor_thread(&ctx, nullptr);

    // ================================================================
    // eBPF 监控器统一启动（由 ServerContext 持有实例，退出时统一 stop）
    // 任务：消除孤儿数据路径，确保每个加载的 BPF 程序有消费者
    // ================================================================

    // DNS 监控器：挂载 kprobe/udp_sendmsg + kprobe/udp_recvmsg
    LOG_INFO(LogModule::NETWORK, "starting DNS monitor thread (interval=10s)");
    start_dns_monitor_thread(&ctx);

    // Wi-Fi 丢包归因：挂载 tracepoint/net/netif_receive_skb 等
    LOG_INFO(LogModule::NETWORK, "starting Wi-Fi packet loss monitor thread (interval=10s)");
    start_wifi_loss_monitor_thread(&ctx);

    // HTTP 请求延迟：挂载 kprobe/tcp_sendmsg + kprobe/tcp_recvmsg
    LOG_INFO(LogModule::NETWORK, "starting HTTP latency monitor thread (interval=10s)");
    start_http_latency_monitor_thread(&ctx);

    // 进程网络画像：挂载 kprobe/tcp_retransmit_skb + kprobe/ip_queue_xmit + kprobe/udp_sendmsg
    // 同探针 tcp_retransmit_skb 双消费者之一（另一个在 TcpLossMonitor），各自独立加载，不合并
    LOG_INFO(LogModule::NETWORK, "starting process net profiler thread (interval=15s)");
    start_process_net_profiler_thread(&ctx);

    // 主线程进入阻塞式 looper
    auto* lp = Looper::current();
    lp->attach(ctx.connection);
    lp->run(&ctx);
    // Looper::run() 退出后，按顺序收尾：
    // 1) 先置 running=false 让所有监控线程退出循环
    // 2) join 全部捕获 ctx* 的线程（含蓝牙/RTT/Jitter/RSSI 及新增句柄），
    //    确保它们完全结束、不再访问 ctx 后才释放资源
    // 3) 各 worker 退出时已自行 stop() 其本地 make_unique 监控器并 store(nullptr)，
    //    故不再在此显式 stop()（避免 worker 正读 map 时资源被销毁的竞态）
    LOG_INFO(LogModule::NETWORK, "server shutting down, stopping monitor threads...");
    ctx.running = false;

    // join 全部监控线程（所有 *_thread 均为 joinable 句柄）
    if (ctx.iface_thread.joinable())                    ctx.iface_thread.join();
    if (ctx.using_thread.joinable())                    ctx.using_thread.join();
    if (ctx.rtt_thread.joinable())                      ctx.rtt_thread.join();
    if (ctx.jitter_thread.joinable())                   ctx.jitter_thread.join();
    if (ctx.rssi_thread.joinable())                     ctx.rssi_thread.join();
    if (ctx.tcp_loss_thread.joinable())                 ctx.tcp_loss_thread.join();
    if (ctx.traffic_analysis_thread.joinable())         ctx.traffic_analysis_thread.join();
    if (ctx.network_quality_thread.joinable())          ctx.network_quality_thread.join();
    if (ctx.bt_thread.joinable())                       ctx.bt_thread.join();
    if (ctx.dns_monitor_thread.joinable())              ctx.dns_monitor_thread.join();
    if (ctx.wifi_loss_monitor_thread.joinable())        ctx.wifi_loss_monitor_thread.join();
    if (ctx.http_latency_monitor_thread.joinable())     ctx.http_latency_monitor_thread.join();
    if (ctx.process_net_profiler_thread.joinable())     ctx.process_net_profiler_thread.join();

    LOG_INFO(LogModule::NETWORK, "all monitor threads joined");

    // 停止文件日志（在 glog 关闭之前）
    Logger::stopFileLog();

    // 清理glog。资源（DBus 连接 / service / weak_mgr）由 ~ServerContext 统一释放。
    google::ShutdownGoogleLogging();

    return 0;
}

}  // namespace weaknet_dbus



