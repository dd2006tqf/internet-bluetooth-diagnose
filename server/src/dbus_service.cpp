/**
 * @file dbus_service.cpp
 * @brief DBus 服务层实现：方法分发、信号发送、载荷持久化
 *
 * 本文件实现 DbusService 类，作为 WeakNet 服务与外部世界交互的唯一入口。
 * 上层通过它暴露约 12 个 DBus 方法（Get、ListInterfaces、HealthCheck、Ping、
 * GetBluetoothDevices、GetEbpfMonitorHealth、GetHistory 等），同时向外发送
 * Changed / NetworkQualityChanged 等 DBus 信号。
 *
 * 设计思路：
 *   - MessageHandler 采用「C 回调 → 静态函数 → 对象方法」三段式，规避 libdbus
 *     纯 C 回调签名不能直接绑定 this 指针的问题
 *   - 每个 handleXxx 方法遵循统一模板：日志 → 解析参数 → 获取依赖 → 构造回复 → 发送
 *   - 信号发送统一走 sendSignalInternal，自带 3 次重试（每次间隔 100ms），
 *     由 send_mutex_ 串行化，保证 DBus 连接上发送顺序与调用顺序一致
 *   - ChangedPayload 在成功发信号后同步持久化到文件，供重启或崩溃后恢复
 *   - 所有结构化返回值（蓝牙、DNS、Wi-Fi、HTTP 等）采用 "key:value|key:value"
 *     扁平编码，避免引入 JSON 库依赖
 *
 * 线程安全：
 *   - 所有 handleXxx 在 DBus 主循环线程中串行执行（libdbus 单线程 dispatching）
 *   - 信号发送由 send_mutex_ 保护，可被多线程并发调用
 *   - ServerContext 中各 monitor 指针仅在启动阶段赋值，之后只读；业务方法调用
 *     时通过 monitor->isAvailable() 或 ctx_ 空指针检查兜底
 */

#include <dbus/dbus.h>
#include <cstdio>
#include <cstring>
#include "logger.hpp"

#include "common.hpp"
#include "serializer.hpp"
#include "server.hpp"
#include "dbus_service.hpp"
#include "weak_netmgr.hpp"
#include "net_info.hpp"
#include "network_quality_result.hpp"
#include "net_ping.h"
#include "bt_monitor.hpp"
#include "bt_audio_analyzer.hpp"
#include "bt_audio_fusion.hpp"
#include "dns_monitor.hpp"
#include "wifi_packet_loss_monitor.hpp"
#include "http_latency_monitor.hpp"
#include "process_net_profiler.hpp"
#include "tcp_retransmit_monitor.hpp"
#include "tcp_conn_monitor.hpp"
#include "skb_drop_monitor.hpp"
#include "weaknet_config.hpp"
#include "utils/json_escape.hpp"
#include "database_manager.hpp"
#include "assurance/legacy_adapter.hpp"
#include "assurance/overall_policy.hpp"
#include "assurance/ip_reachability_evaluator.hpp"
#include "assurance/responsiveness_evaluator.hpp"
#include "assurance/reliability_evaluator.hpp"
#include "assurance/rf_health_evaluator.hpp"
#include "assessment_snapshot.hpp"
#include "assurance/dns_service_evaluator.hpp"
#include "assurance/tcp_connect_evaluator.hpp"
#include "assurance/http_access_evaluator.hpp"
#include "assurance/captive_portal_evaluator.hpp"
#include "assurance/active_connectivity.hpp"
#include "tcp_connect_monitor.hpp"
#include "active_connectivity_monitor.hpp"
#include <sstream>

namespace weaknet_dbus {

DbusService::DbusService(ServerContext* ctx) : ctx_(ctx) {}

/**
 * @brief libdbus C 回调入口 → 转调到具体成员方法
 * @param conn     DBus 连接
 * @param msg      收到的消息（方法调用）
 * @param user_data 注册时传入的 this 指针
 * @return DBUS_HANDLER_RESULT_HANDLED 表示已处理；NOT_YET_HANDLED 表示交给下一个处理器
 *
 * 每个 if 分支对应一个公开的 DBus 方法；匹配到接口名 + 方法名后直接调用
 * 对应的 handleXxx 成员。未匹配的方法返回 NOT_YET_HANDLED，让 libdbus 继续寻找
 * 其他注册的处理器（如果有的话）。
 */
static DBusHandlerResult MessageHandlerStatic(DBusConnection* conn, DBusMessage* msg, void* user_data) {
    auto* self = reinterpret_cast<DbusService*>(user_data);
    if (!self) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    if (dbus_message_is_method_call(msg, kInterface, kMethodGet)) {
        self->handleGet(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, kInterface, kMethodListInterfaces)) {
        self->handleListInterfaces(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    // GetInterfaces 是 ListInterfaces 的别名，行为完全一致
    if (dbus_message_is_method_call(msg, kInterface, kMethodGetInterfaces)) {
        self->handleListInterfaces(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, kInterface, kMethodHealthCheck)) {
        self->handleHealthCheck(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, kInterface, kMethodGetDiagnosis)) {
        self->handleGetDiagnosis(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, kInterface, kMethodExecuteAction)) {
        self->handleExecuteAction(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, kInterface, kMethodPing)) {
        self->handlePing(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, kInterface, kMethodGetBluetoothDevices)) {
        self->handleGetBluetoothDevices(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, kInterface, kMethodGetBluetoothAdapter)) {
        self->handleGetBluetoothAdapter(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, kInterface, kMethodGetBluetoothAudioQuality)) {
        self->handleGetBluetoothAudioQuality(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, kInterface, kMethodGetCoexistenceConflict)) {
        self->handleGetCoexistenceConflict(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, kInterface, kMethodGetNetworkExperience)) {
        self->handleGetNetworkExperience(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, kInterface, kMethodGetDnsStats)) {
        self->handleGetDnsStats(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, kInterface, kMethodGetWifiLossStats)) {
        self->handleGetWifiLossStats(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, kInterface, kMethodGetHttpLatencyStats)) {
        self->handleGetHttpLatencyStats(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, kInterface, kMethodGetProcessProfiling)) {
        self->handleGetProcessProfiling(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, kInterface, kMethodGetSkbDropStats)) {
        self->handleGetSkbDropStats(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, kInterface, kMethodGetEbpfMonitorHealth)) {
        self->handleGetEbpfMonitorHealth(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, kInterface, kMethodGetHistory)) {
        self->handleGetHistory(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, kInterface, kMethodSetMonitorParam)) {
        self->handleSetMonitorParam(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, kInterface, kMethodGetMonitorParam)) {
        self->handleGetMonitorParam(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, kInterface, kMethodListMonitors)) {
        self->handleListMonitors(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, kInterface, kMethodGetMonitorStatus)) {
        self->handleGetMonitorStatus(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, kInterface, kMethodEnableMonitor)) {
        self->handleEnableMonitor(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, kInterface, kMethodDisableMonitor)) {
        self->handleDisableMonitor(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, kInterface, kMethodRestartMonitor)) {
        self->handleRestartMonitor(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, kInterface, kMethodSaveMonitorOverrides)) {
        self->handleSaveMonitorOverrides(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/**
 * @brief 在 DBus 连接上注册对象路径 vtable
 * @param conn DBus 连接
 * @return true 注册成功；false 连接无效或已存在 vtable
 *
 * libdbus 采用 vtable 模式：一个对象路径对应一个 vtable，其中 message_function
 * 就是上面的静态转发函数。这样每条方法调用都会被路由到 MessageHandlerStatic。
 */
bool DbusService::register_on_connection(DBusConnection* conn) {
    static DBusObjectPathVTable vtable{};
    vtable.message_function = &MessageHandlerStatic;
    return dbus_connection_register_object_path(conn, kObjectPath, &vtable, this);
}

/**
 * @brief 内部统一信号发送函数（带重试）
 * @param signalName DBus 信号名（如 kSignalChanged、kSignalNetworkQualityChanged）
 * @param args       信号参数列表，每项为 {DBUS_TYPE_*, 值指针}
 * @param counter    事件计数器，便于接收方去重/排序
 * @return true 发送成功；false 三次重试全部失败
 *
 * 重试策略：最多 3 次，失败间隔 100ms。每次迭代：创建 signal → 追加参数 → send → flush。
 * 若 create signal 或 append 参数失败直接返回，不重试（属于编程错误而非网络抖动）。
 */
bool DbusService::sendSignalInternal(const std::string& signalName,
                                    const std::vector<std::pair<int, const void*>>& args,
                                    int32_t counter) {
    if (!ctx_ || !ctx_->connection) return false;

    const int max_retries = 3;
    const int retry_delay_ms = 100;

    for (int attempt = 0; attempt < max_retries; ++attempt) {
        DBusMessage* signal = dbus_message_new_signal(kObjectPath, kInterface, signalName.c_str());
        if (!signal) {
            LOG_ERROR(LogModule::DBUS, "sendSignalInternal: failed to create signal " << signalName);
            return false;
        }

        DBusMessageIter iter;
        dbus_message_iter_init_append(signal, &iter);

        // 按顺序逐个追加参数；args 中的指针在调用栈上，生命周期覆盖整个循环迭代
        for (const auto& [type, value] : args) {
            if (!dbus_message_iter_append_basic(&iter, type, value)) {
                LOG_ERROR(LogModule::DBUS, "sendSignalInternal: failed to append argument for " << signalName);
                dbus_message_unref(signal);
                return false;
            }
        }

        bool ok = dbus_connection_send(ctx_->connection, signal, nullptr);
        dbus_connection_flush(ctx_->connection);
        dbus_message_unref(signal);

        if (ok) {
            LOG_INFO(LogModule::DBUS, "sendSignalInternal: emitted " << signalName << " counter=" << counter);
            return true;
        }

        LOG_WARNING(LogModule::DBUS, "sendSignalInternal(" << signalName << "): attempt " << attempt + 1
                    << " failed, retrying in " << retry_delay_ms << "ms");

        if (attempt < max_retries - 1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(retry_delay_ms));
        }
    }

    LOG_ERROR(LogModule::DBUS, "sendSignalInternal(" << signalName << "): all " << max_retries << " attempts failed");
    return false;
}

/**
 * @brief 发送通用 Changed 信号（带持久化）
 * @param message 信号载荷字符串
 * @param counter 事件计数器
 * @return true 发送成功
 *
 * 与 emitSpecificSignal 的区别：成功发送后额外将载荷序列化到文件，供重启恢复。
 * 内部持 send_mutex_，保证与其他信号发送的串行顺序。
 */
bool DbusService::emitChanged(const std::string& message, int32_t counter) {
    std::lock_guard<std::mutex> lock(send_mutex_);

    // 构造参数列表
    const char* s = message.c_str();
    std::vector<std::pair<int, const void*>> args = {
        {DBUS_TYPE_STRING, &s},
        {DBUS_TYPE_INT32, &counter}
    };

    bool ok = sendSignalInternal(kSignalChanged, args, counter);

    // 仅在信号发送成功时持久化，避免文件与 DBus 状态不一致
    if (ok) {
        ChangedPayload payload{message, counter};
        std::string err;
        serializeChangedPayloadToFile(payload, kSignalSerializedFile, &err);
    }

    return ok;
}

// MessageHandler 实现已移动到静态自由函数（见文件顶部 MessageHandlerStatic）

/**
 * @brief DBus 方法实现：Get —— 健康检查接口，返回固定字符串
 * @param conn DBus 连接
 * @param msg 接收到的方法调用消息
 * @return true 回复发送成功
 *
 * 同时将回复内容序列化到 kGetReplySerializedFile，便于非 DBus 客户端读取。
 */
bool DbusService::handleGet(DBusConnection* conn, DBusMessage* msg) {
    LOG_INFO(LogModule::DBUS, "handleGet called");
    const char* reply_text = "Hello from WeakNet Server";
    DBusMessage* reply = dbus_message_new_method_return(msg);
    if (!reply) {
        LOG_ERROR(LogModule::DBUS, "handleGet: failed to create reply");
        return false;
    }
    DBusMessageIter args;
    dbus_message_iter_init_append(reply, &args);
    const char* s = reply_text;
    if (!dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &s)) {
        LOG_ERROR(LogModule::DBUS, "handleGet: failed to append message");
        dbus_message_unref(reply);
        // 参数追加失败时，主动发送一个错误回复，让客户端收到明确的失败原因
        DBusMessage* error_reply = dbus_message_new_error(msg, "com.example.WeakNet.Error", "Failed to append message");
        if (error_reply) {
            dbus_connection_send(conn, error_reply, nullptr);
            dbus_connection_flush(conn);
            dbus_message_unref(error_reply);
        }
        return false;
    }
    if (!dbus_connection_send(conn, reply, nullptr)) {
        LOG_ERROR(LogModule::DBUS, "handleGet: failed to send reply");
        dbus_message_unref(reply);
        return false;
    }
    dbus_connection_flush(conn);
    dbus_message_unref(reply);

    // 契约保证：将回复持久化到离线序列化文件，供 weaknet_get_from_file 读取
    std::string err;
    serializeGetReplyToFile(reply_text, kGetReplySerializedFile, &err);

    return true;
}

/**
 * @brief 内部辅助：向调用方返回 string 数组
 * @param conn DBus 连接
 * @param msg 接收到的方法调用消息
 * @param arr 待返回的字符串列表
 * @return true 回复发送成功
 *
 * libdbus 中发送数组需要三步：open_container → append_basic × N → close_container。
 * 本方法将此模板封装，所有返回字符串列表的 handleXxx（如 ListInterfaces、
 * GetBluetoothDevices）都复用它。
 */
bool DbusService::replyStringArray(DBusConnection* conn, DBusMessage* msg, const std::vector<std::string>& arr) {
    DBusMessage* reply = dbus_message_new_method_return(msg);
    if (!reply) {
        LOG_ERROR(LogModule::DBUS, "replyStringArray: failed to create reply");
        return false;
    }
    DBusMessageIter iter;
    dbus_message_iter_init_append(reply, &iter);
    DBusMessageIter array_iter;
    // DBUS_TYPE_STRING_AS_STRING = "s"，用于告诉 dbus_message_iter_open_container 数组元素类型
    if (!dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, DBUS_TYPE_STRING_AS_STRING, &array_iter)) {
        LOG_ERROR(LogModule::DBUS, "replyStringArray: failed to open array container");
        dbus_message_unref(reply);
        return false;
    }
    for (const auto& s : arr) {
        const char* cs = s.c_str();
        if (!dbus_message_iter_append_basic(&array_iter, DBUS_TYPE_STRING, &cs)) {
            LOG_ERROR(LogModule::DBUS, "replyStringArray: failed to append string");
            dbus_message_iter_close_container(&iter, &array_iter);
            dbus_message_unref(reply);
            return false;
        }
    }
    if (!dbus_message_iter_close_container(&iter, &array_iter)) {
        LOG_ERROR(LogModule::DBUS, "replyStringArray: failed to close array container");
        dbus_message_unref(reply);
        return false;
    }
    bool ok = dbus_connection_send(conn, reply, nullptr);
    dbus_connection_flush(conn);
    dbus_message_unref(reply);
    return ok;
}

/**
 * @brief DBus 方法实现：ListInterfaces —— 返回当前所有网卡接口名
 * @param conn DBus 连接
 * @param msg 接收到的方法调用消息
 * @return true 回复发送成功
 *
 * 数据源来自 WeakNetMgr::current_interfaces_（线程安全 getCurrentInterfaces()），
 * 返回值是接口名的字符串数组。
 */
bool DbusService::handleListInterfaces(DBusConnection* conn, DBusMessage* msg) {
    LOG_INFO(LogModule::DBUS, "handleListInterfaces called");
    // 接口列表唯一事实源 = WeakNetMgr::current_interfaces_（线程安全接口）
    std::vector<NetInfo> snapshot = ctx_->weak_mgr->getCurrentInterfaces();
    return replyStringArray(conn, msg, WeakNetMgr::namesOf(snapshot));
}

/**
 * @brief DBus 方法实现：HealthCheck —— 返回当前网络质量评估
 * @param conn DBus 连接
 * @param msg 接收到的方法调用消息
 * @return true 回复发送成功
 *
 * 构造临时 NetworkQualityAssessor，对当前网卡快照做一次性评估，结果以字符串返回。
 * 不缓存 assessor，保证每次调用都是最新快照。
 */
bool DbusService::handleHealthCheck(DBusConnection* conn, DBusMessage* msg) {
    LOG_INFO(LogModule::DBUS, "handleHealthCheck called (CR-1 LegacyAdapter route)");

    // 无上行时不编造网卡名（此前默认 "wlan0"）：返回的 interface 字段必须
    // 要么是真实在用的网卡，要么为空并由 overall=UNKNOWN 表达"无结论"。
    std::string active_iface;
    if (ctx_ && ctx_->weak_mgr) {
        auto opt = ctx_->weak_mgr->getCurrentUsingInterface();
        if (opt.has_value()) active_iface = opt.value();
    }

    // 从 MetricsRegistry 拉取窗口并使用无状态 Evaluator 评估
    weaknet::NetworkExperience exp;
    int rtt_val = -1;
    double tcp_loss_val = 0.0;
    int rssi_val = -1000;
    double jitter_val = 0.0;
    double median_rtt_val = -1.0;
    std::string resp_reason = "";

    // 无上行网卡时不查 registry（空串查不到任何样本），直接给显式 UNKNOWN：
    // 返回一份"不知道"比返回一份基于无关网卡的结论更诚实。
    if (ctx_ && ctx_->metrics_registry && !active_iface.empty()) {
        using namespace std::chrono_literals;
        auto reach_samples = ctx_->metrics_registry->window(active_iface, weaknet::MetricId::REACHABILITY_SUCCESS, 120s);
        auto rtt_samples = ctx_->metrics_registry->window(active_iface, weaknet::MetricId::RTT_MS, 120s);
        auto jitter_samples = ctx_->metrics_registry->window(active_iface, weaknet::MetricId::JITTER_MS, 120s);
        auto wifi_samples = ctx_->metrics_registry->window(active_iface, weaknet::MetricId::WIFI_LOSS_RATE, 120s);
        auto tcp_samples = ctx_->metrics_registry->window(active_iface, weaknet::MetricId::TCP_LOSS_RATE, 120s);
        auto rssi_samples = ctx_->metrics_registry->window(active_iface, weaknet::MetricId::RSSI_DBM, 120s);

        bool is_wireless = (active_iface.rfind("wl", 0) == 0);
        auto reach_sle = weaknet::IpReachabilityEvaluator::evaluate(reach_samples);
        auto resp_sle = weaknet::ResponsivenessEvaluator::evaluate(rtt_samples, jitter_samples);
        auto rel_sle = weaknet::ReliabilityEvaluator::evaluate(wifi_samples, tcp_samples, is_wireless);
        auto rf_sle = weaknet::RfHealthEvaluator::evaluate(rssi_samples, is_wireless);
        // W2 单一事实源：HealthCheck **只读权威快照**，绝不重新 evaluate。
        // 此前本方法现场拉 metrics + 调 OverallPolicy，与 quality 线程、
        // history 线程构成三条结论可能不一致的评估路径（既有 bug）。
        auto snap = ctx_->assessment_store.latest();
        if (snap) {
            const uint64_t cur_epoch = ctx_->dns_tracker ? ctx_->dns_tracker->currentBindingEpoch()
                                                         : ctx_->dns_binding_epoch.load();
            if (weaknet::AssessmentSnapshotStore::isCurrent(*snap,
                    ctx_->cfg.config_generation.load(), cur_epoch)) {
                exp = snap->experience;
            } else {
                // snapshot 过期：显式 UNKNOWN，等下一轮评估，不返回旧 profile 结论
                exp.iface = active_iface;
                exp.overall = weaknet::HealthState::UNKNOWN;
                exp.overall_coverage = weaknet::Coverage::NONE;
                exp.primary_issue = "stale_assessment";
            }
        } else {
            exp.iface = active_iface;
            exp.overall = weaknet::HealthState::UNKNOWN;
            exp.display_score = 50;
            exp.primary_issue = "no_assessment_yet";
        }
        resp_reason = resp_sle.reason;
        for (const auto& ev : resp_sle.evidence) {
            if (ev.metric == "median_rtt_ms") {
                median_rtt_val = ev.value;
            }
        }

        auto latest_rtt = ctx_->metrics_registry->latest(active_iface, weaknet::MetricId::RTT_MS);
        if (latest_rtt.has_value() && latest_rtt->state == weaknet::MetricState::VALID) rtt_val = static_cast<int>(latest_rtt->value);

        auto latest_loss = ctx_->metrics_registry->latest(active_iface, weaknet::MetricId::TCP_LOSS_RATE);
        if (latest_loss.has_value() && latest_loss->state == weaknet::MetricState::VALID) tcp_loss_val = latest_loss->value;

        auto latest_rssi = ctx_->metrics_registry->latest(active_iface, weaknet::MetricId::RSSI_DBM);
        if (latest_rssi.has_value() && latest_rssi->state == weaknet::MetricState::VALID) rssi_val = static_cast<int>(latest_rssi->value);

        auto latest_jitter = ctx_->metrics_registry->latest(active_iface, weaknet::MetricId::JITTER_MS);
        if (latest_jitter.has_value() && latest_jitter->state == weaknet::MetricState::VALID) jitter_val = latest_jitter->value;
    } else {
        exp.iface = active_iface;
        exp.overall = weaknet::HealthState::UNKNOWN;
        exp.display_score = 50;
    }

    std::string reply_text = weaknet::LegacyAdapter::toHealthCheckJson(exp, rtt_val, tcp_loss_val, rssi_val, jitter_val, median_rtt_val, resp_reason);

    DBusMessage* reply = dbus_message_new_method_return(msg);
    if (!reply) return false;
    DBusMessageIter args;
    dbus_message_iter_init_append(reply, &args);
    const char* s = reply_text.c_str();
    if (!dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &s)) { dbus_message_unref(reply); return false; }
    if (!dbus_connection_send(conn, reply, nullptr)) { dbus_message_unref(reply); return false; }
    dbus_connection_flush(conn);
    dbus_message_unref(reply);
    return true;
}

/**
 * @brief DBus 方法实现：GetDiagnosis —— 返回端侧确定性机器诊断事实 JSON
 */
bool DbusService::handleGetDiagnosis(DBusConnection* conn, DBusMessage* msg) {
    LOG_INFO(LogModule::DBUS, "handleGetDiagnosis called");

    std::string reply_text = "{}";
    if (ctx_ && ctx_->diagnosis_engine && ctx_->evidence_id_generator) {
        auto snap = ctx_->assessment_store.latest();
        if (snap) {
            auto facts = ctx_->diagnosis_engine->diagnose(*snap, *ctx_->evidence_id_generator);
            reply_text = facts.toJson();
        } else {
            weaknet::DiagnosisFacts fallback;
            fallback.fault_domain = "NONE";
            fallback.primary_issue = "snapshot_not_ready";
            fallback.confidence = weaknet::DiagnosisConfidence::LOW;
            fallback.default_summary_template = "系统监控评估快照正在采集中，尚未生成，请稍候。";
            reply_text = fallback.toJson();
        }
    } else {
        weaknet::DiagnosisFacts fallback;
        fallback.fault_domain = "NONE";
        fallback.primary_issue = "diagnosis_engine_not_initialized";
        fallback.confidence = weaknet::DiagnosisConfidence::LOW;
        fallback.default_summary_template = "端侧诊断引擎尚未初始化就绪。";
        reply_text = fallback.toJson();
    }

    DBusMessage* reply = dbus_message_new_method_return(msg);
    if (!reply) return false;
    DBusMessageIter args;
    dbus_message_iter_init_append(reply, &args);
    const char* s = reply_text.c_str();
    if (!dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &s)) { dbus_message_unref(reply); return false; }
    if (!dbus_connection_send(conn, reply, nullptr)) { dbus_message_unref(reply); return false; }
    dbus_connection_flush(conn);
    dbus_message_unref(reply);
    return true;
}

/**
 * @brief 校验调用方为 root (UID 0)，否则回 ACCESS_DENIED。
 *
 * 覆盖面：一切能改变设备行为的写方法。理由见头文件声明。
 * 取不到 sender 或 UID 查询失败时**保守拒绝**——把"无法确定身份"当作非特权，
 * 而不是放行。
 */
bool DbusService::requireRootCaller(DBusConnection* conn, DBusMessage* msg,
                                    const char* method_name) {
    const char* sender = dbus_message_get_sender(msg);
    unsigned long caller_uid = 1000;  // 默认按普通用户处理
    DBusError uid_err;
    dbus_error_init(&uid_err);
    if (sender) {
        caller_uid = dbus_bus_get_unix_user(conn, sender, &uid_err);
        if (dbus_error_is_set(&uid_err)) {
            LOG_WARNING(LogModule::DBUS, method_name
                        << ": failed to get caller UID: " << uid_err.message);
            dbus_error_free(&uid_err);
            caller_uid = 1000;
        }
    }
    if (caller_uid == 0) return true;

    LOG_ERROR(LogModule::DBUS, method_name << " rejected: caller UID " << caller_uid
              << " != 0 (permission denied)");
    DBusMessage* reply = dbus_message_new_error(msg, DBUS_ERROR_ACCESS_DENIED,
        "Permission denied: only root (UID 0) is authorized to modify WeakNet state");
    if (reply) {
        dbus_connection_send(conn, reply, nullptr);
        dbus_message_unref(reply);
    }
    return false;
}

/**
 * @brief DBus 方法实现：ExecuteAction —— 安全执行白名单建议动作
 */
bool DbusService::handleExecuteAction(DBusConnection* conn, DBusMessage* msg) {
    LOG_INFO(LogModule::DBUS, "handleExecuteAction called");

    // 严格安全防线：校验调用者 UID 必须为 root (UID 0)，防止非特权本地用户通过系统总线触发特权网络动作
    if (!requireRootCaller(conn, msg, "ExecuteAction")) return false;

    DBusError err;
    dbus_error_init(&err);
    const char* action_id = nullptr;
    const char* param_key = nullptr;
    const char* param_val = nullptr;

    // 支持参数格式：action_id, param_key, param_val (空则忽略)
    if (!dbus_message_get_args(msg, &err, DBUS_TYPE_STRING, &action_id,
                               DBUS_TYPE_STRING, &param_key,
                               DBUS_TYPE_STRING, &param_val,
                               DBUS_TYPE_INVALID)) {
        dbus_error_free(&err);
        // 也尝试仅单个 action_id
        dbus_error_init(&err);
        if (!dbus_message_get_args(msg, &err, DBUS_TYPE_STRING, &action_id, DBUS_TYPE_INVALID)) {
            dbus_error_free(&err);
            DBusMessage* reply = dbus_message_new_error(msg, "com.example.WeakNet.Error", "Invalid arguments: expected action_id");
            dbus_connection_send(conn, reply, nullptr);
            dbus_message_unref(reply);
            return false;
        }
    }

    std::map<std::string, std::string> params;
    if (param_key && strlen(param_key) > 0 && param_val) {
        params[param_key] = param_val;
    }

    // 所有内插字符串一律经 escapeJsonString：命令输出（如 /etc/resolv.conf 的
    // 注释与换行、ip route 的缩进）与 caller 提供的 action_id 都可能含引号或
    // 控制字符，裸拼进 JSON 字面量会产出客户端无法解析的畸形 JSON。
    std::string result_json;
    if (ctx_ && ctx_->action_registry) {
        auto val_res = ctx_->action_registry->validate(action_id, params);
        if (!val_res.ok) {
            result_json = "{\"success\":false,\"error\":\""
                        + weaknet_utils::escapeJsonString(val_res.error) + "\"}";
        } else {
            auto spec_opt = ctx_->action_registry->buildExecSpec(action_id, params);
            if (!spec_opt.has_value()) {
                result_json = "{\"success\":false,\"error\":\"Failed to build ExecSpec\"}";
            } else {
                auto exec_res = weaknet::ActionRegistry::safeExec(*spec_opt);
                result_json = "{\"success\":" + std::string(exec_res.exit_code == 0 ? "true" : "false")
                            + ",\"exit_code\":" + std::to_string(exec_res.exit_code)
                            + ",\"stdout\":\""
                            + weaknet_utils::escapeJsonString(exec_res.stdout_output.substr(0, 500)) + "\""
                            + ",\"error\":\""
                            + weaknet_utils::escapeJsonString(exec_res.error) + "\"}";
            }
        }
    } else {
        result_json = "{\"success\":false,\"error\":\"ActionRegistry not initialized\"}";
    }

    DBusMessage* reply = dbus_message_new_method_return(msg);
    if (!reply) return false;
    DBusMessageIter args;
    dbus_message_iter_init_append(reply, &args);
    const char* s = result_json.c_str();
    if (!dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &s)) { dbus_message_unref(reply); return false; }
    if (!dbus_connection_send(conn, reply, nullptr)) { dbus_message_unref(reply); return false; }
    dbus_connection_flush(conn);
    dbus_message_unref(reply);
    return true;
}

/**
 * @brief 发送带计数器的指定 DBus 信号（通用两参数版）
 * @param signalName 具体信号名（如 kSignalInterfaceChanged）
 * @param message    信号消息体
 * @param counter    事件计数器
 * @return true 发送成功
 *
 * 与 emitChanged 的区别：不持久化；与 emitNetworkQualitySignal 的区别：少一个 details 参数。
 * 内部持 send_mutex_，保证发送顺序。
 */
bool DbusService::emitSpecificSignal(const std::string& signalName, const std::string& message, int32_t counter) {
    std::lock_guard<std::mutex> lock(send_mutex_);

    // 构造参数列表
    const char* msg = message.c_str();
    std::vector<std::pair<int, const void*>> args = {
        {DBUS_TYPE_STRING, &msg},
        {DBUS_TYPE_INT32, &counter}
    };

    return sendSignalInternal(signalName, args, counter);
}

/**
 * @brief 发送网络质量变更信号（三参数版：message + details + counter）
 * @param message 简短质量描述
 * @param details 详细质量评估（JSON 或长文本）
 * @param counter 事件计数器
 * @return true 发送成功
 */
bool DbusService::emitNetworkQualitySignal(const std::string& message, const std::string& details, int32_t counter) {
    std::lock_guard<std::mutex> lock(send_mutex_);

    // 构造参数列表
    const char* quality = message.c_str();
    const char* details_str = details.c_str();
    std::vector<std::pair<int, const void*>> args = {
        {DBUS_TYPE_STRING, &quality},
        {DBUS_TYPE_STRING, &details_str},
        {DBUS_TYPE_INT32, &counter}
    };

    return sendSignalInternal(kSignalNetworkQualityChanged, args, counter);
}

/**
 * @brief DBus 方法实现：Ping —— 对指定主机执行 ICMP 探测
 * @param conn DBus 连接
 * @param msg 接收到的方法调用消息（参数：hostname 字符串）
 * @return true 回复发送成功
 *
 * 步骤：解析 hostname → 找到当前上网网卡 → 调用 NetPing::ping()（3 秒超时）→ 回复结果字符串。
 * 任何环节失败都返回 DBus error 消息而非空回复。
 */
bool DbusService::handlePing(DBusConnection* conn, DBusMessage* msg) {
    LOG_INFO(LogModule::DBUS, "handlePing called");

    // 解析参数：目标主机名
    DBusError err;
    dbus_error_init(&err);
    const char* hostname = nullptr;

    if (!dbus_message_get_args(msg, &err, DBUS_TYPE_STRING, &hostname, DBUS_TYPE_INVALID)) {
        LOG_ERROR(LogModule::DBUS, "Ping method error: " << err.message);
        dbus_error_free(&err);

        // 发送错误回复
        DBusMessage* reply = dbus_message_new_error(msg, "com.example.WeakNet.Error", "Invalid arguments");
        dbus_connection_send(conn, reply, nullptr);
        dbus_message_unref(reply);
        return false;
    }

    if (!hostname || strlen(hostname) == 0) {
        LOG_ERROR(LogModule::DBUS, "Ping method error: empty hostname");

        // 发送错误回复
        DBusMessage* reply = dbus_message_new_error(msg, "com.example.WeakNet.Error", "Empty hostname");
        dbus_connection_send(conn, reply, nullptr);
        dbus_message_unref(reply);
        return false;
    }

    LOG_INFO(LogModule::DBUS, "Ping request for host: " << hostname);

    // 获取当前上网网卡（接口列表唯一事实源 = WeakNetMgr::current_interfaces_）
    std::string currentIface;
    {
        auto interfaces = ctx_->weak_mgr->getCurrentInterfaces();
        for (const auto& net : interfaces) {
            if (net.usingNow()) {
                currentIface = net.ifName();
                break;
            }
        }
    }

    if (currentIface.empty()) {
        LOG_ERROR(LogModule::DBUS, "Ping method error: no active interface found");

        // 发送错误回复
        DBusMessage* reply = dbus_message_new_error(msg, "com.example.WeakNet.Error", "No active network interface");
        dbus_connection_send(conn, reply, nullptr);
        dbus_message_unref(reply);
        return false;
    }

    LOG_INFO(LogModule::DBUS, "Using interface: " << currentIface << " for ping to " << hostname);

    // 调用NetPing进行ping测试
    auto pingInstance = NetPing::getInstance();
    int pingResult = pingInstance->ping(hostname, currentIface, 3000); // 3秒超时

    // 构建回复消息
    DBusMessage* reply = dbus_message_new_method_return(msg);
    if (!reply) {
        LOG_ERROR(LogModule::DBUS, "Failed to create ping reply message");
        return false;
    }

    DBusMessageIter args;
    dbus_message_iter_init_append(reply, &args);

    // 根据 ping 返回值（≥0 为 RTT ms，<0 为错误码）构造人类可读字符串
    std::string result;
    if (pingResult >= 0) {
        result = std::string("PING ") + hostname + " via " + currentIface + ": " + std::to_string(pingResult) + "ms";
        LOG_INFO(LogModule::DBUS, "Ping successful: " << result);
    } else {
        result = std::string("PING ") + hostname + " via " + currentIface + ": FAILED (error code: " + std::to_string(pingResult) + ")";
        LOG_INFO(LogModule::DBUS, "Ping failed: " << result);
    }

    const char* resultStr = result.c_str();
    if (!dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &resultStr)) {
        LOG_ERROR(LogModule::DBUS, "Failed to append ping result to reply");
        dbus_message_unref(reply);
        return false;
    }

    // 发送回复
    bool ok = dbus_connection_send(conn, reply, nullptr);
    dbus_connection_flush(conn);
    dbus_message_unref(reply);

    LOG_INFO(LogModule::DBUS, "Ping reply sent: " << (ok ? "success" : "failed"));
    return ok;
}

// ============================================================================
// 蓝牙设备相关方法
// ============================================================================

/**
 * @brief DBus 方法实现：GetBluetoothDevices —— 返回蓝牙设备列表
 * @param conn DBus 连接
 * @param msg 接收到的方法调用消息
 * @return true 回复发送成功
 *
 * 每个设备编码为一条 "MAC|Name|RSSI|Connected|Type|Level" 字符串，
 * 所有设备组成 string 数组返回。BtMonitor 未初始化时返回空数组。
 */
bool DbusService::handleGetBluetoothDevices(DBusConnection* conn, DBusMessage* msg) {
    LOG_INFO(LogModule::DBUS, "handleGetBluetoothDevices called");

    BtMonitor* monitor = ctx_ ? ctx_->bt_monitor : nullptr;
    if (!monitor) {
        // 无蓝牙监测器 → 返回空数组。必须构造合法的空 DBus 数组容器，不能跳过 open_container
        DBusMessage* reply = dbus_message_new_method_return(msg);
        if (reply) {
            DBusMessageIter iter;
            dbus_message_iter_init_append(reply, &iter);
            DBusMessageIter arr;
            dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, DBUS_TYPE_STRING_AS_STRING, &arr);
            dbus_message_iter_close_container(&iter, &arr);
            dbus_connection_send(conn, reply, nullptr);
            dbus_connection_flush(conn);
            dbus_message_unref(reply);
        }
        return true;
    }

    // 获取设备列表，格式化为 "MAC|Name|RSSI|Connected|Type|Level" 字符串
    auto devices = monitor->getDevices();
    std::vector<std::string> lines;
    lines.reserve(devices.size());
    for (const auto& dev : devices) {
        // 字段顺序固定：MAC → 显示名（优先 alias，无则 name）→ RSSI dBm → 连接标记 → 类型 → RSSI 等级
        std::string line = dev.macAddress + "|"
            + (dev.name.empty() ? dev.alias : dev.name) + "|"
            + std::to_string(dev.rssiDbm) + "|"
            + (dev.connected ? "1" : "0") + "|"
            + (dev.deviceType == BtDeviceType::BLE ? "BLE" :
               dev.deviceType == BtDeviceType::Classic ? "Classic" : "Dual") + "|"
            + dev.rssiLevel();
        lines.push_back(line);
    }
    return replyStringArray(conn, msg, lines);
}

/**
 * @brief DBus 方法实现：GetBluetoothAdapter —— 返回本机蓝牙适配器状态
 * @param conn DBus 连接
 * @param msg 接收到的方法调用消息
 * @return true 回复发送成功
 *
 * 编码格式："Powered:0/1|Name:xxx|Address:XX:XX:...|Discovering:0/1|Discoverable:0/1|Pairable:0/1"
 * BtMonitor 未初始化时返回 "No Bluetooth adapter available"。
 */
bool DbusService::handleGetBluetoothAdapter(DBusConnection* conn, DBusMessage* msg) {
    LOG_INFO(LogModule::DBUS, "handleGetBluetoothAdapter called");

    DBusMessage* reply = dbus_message_new_method_return(msg);
    if (!reply) return false;

    std::string result;
    BtMonitor* monitor = ctx_ ? ctx_->bt_monitor : nullptr;
    if (monitor && monitor->isInitialized()) {
        auto state = monitor->getAdapterState();
        result = std::string("Powered:") + (state.powered ? "1" : "0")
            + "|Name:" + state.name
            + "|Address:" + state.macAddress
            + "|Discovering:" + (state.discovering ? "1" : "0")
            + "|Discoverable:" + (state.discoverable ? "1" : "0")
            + "|Pairable:" + (state.pairable ? "1" : "0");
    } else {
        result = "No Bluetooth adapter available";
    }

    DBusMessageIter args;
    dbus_message_iter_init_append(reply, &args);
    const char* s = result.c_str();
    dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &s);
    dbus_connection_send(conn, reply, nullptr);
    dbus_connection_flush(conn);
    dbus_message_unref(reply);
    return true;
}

/**
 * @brief DBus 方法实现：GetBluetoothAudioQuality —— 返回指定设备音频质量与 eBPF 融合诊断 JSON
 * @param conn DBus 连接
 * @param msg  接收到的方法调用消息（可选参数 string: macAddress）
 */
bool DbusService::handleGetBluetoothAudioQuality(DBusConnection* conn, DBusMessage* msg) {
    LOG_INFO(LogModule::DBUS, "handleGetBluetoothAudioQuality called");
    DBusMessage* reply = dbus_message_new_method_return(msg);
    if (!reply) return false;

    std::string targetMac;
    DBusError err;
    dbus_error_init(&err);
    const char* reqMac = nullptr;
    if (dbus_message_get_args(msg, &err, DBUS_TYPE_STRING, &reqMac, DBUS_TYPE_INVALID)) {
        if (reqMac) targetMac = reqMac;
    } else {
        dbus_error_free(&err);
    }

    BtMonitor* monitor = ctx_ ? ctx_->bt_monitor : nullptr;
    std::string jsonResult;

    if (!monitor || !monitor->isInitialized()) {
        jsonResult = "{\"error\":\"Bluetooth monitor not available or uninitialized\",\"mac\":\"" + targetMac + "\"}";
    } else {
        // 如果未指定 targetMac，则尝试寻找第一个已连接设备或已有的 transport 设备
        if (targetMac.empty()) {
            auto transports = monitor->getAudioTransports();
            if (!transports.empty()) {
                targetMac = transports.front().deviceMac;
            } else {
                auto connected = monitor->getConnectedDevices();
                if (!connected.empty()) {
                    targetMac = connected.front().macAddress;
                }
            }
        }

        if (targetMac.empty()) {
            jsonResult = "{\"error\":\"No active Bluetooth device specified or found\",\"mac\":\"\"}";
        } else {
            BtAudioFusionResult fusion;
            bool found = monitor->getAudioFusionResult(targetMac, &fusion);
            if (!found) {
                // 如果无 transport，仍尝试获取设备基本信息
                BtDeviceInfo devInfo;
                bool devFound = monitor->getDevice(targetMac, &devInfo);
                jsonResult = "{\"mac\":\"" + targetMac + "\","
                             "\"name\":\"" + (devFound ? (devInfo.name.empty() ? devInfo.alias : devInfo.name) : "unknown") + "\","
                             "\"connected\":" + (devFound && devInfo.connected ? "true" : "false") + ","
                             "\"audio_transport_active\":false,"
                             "\"quality_score\":0.0,"
                             "\"quality_level\":\"unknown\","
                             "\"diagnostic\":\"No A2DP media transport detected for this device\"}";
            } else {
                std::ostringstream oss;
                oss << "{"
                    << "\"mac\":\"" << fusion.deviceMac << "\","
                    << "\"active\":" << (fusion.isActive ? "true" : "false") << ","
                    << "\"effective_active\":" << (fusion.effectiveActive ? "true" : "false") << ","
                    << "\"suspected_stall\":" << (fusion.suspectedStall ? "true" : "false") << ","
                    << "\"quality_score\":" << fusion.qualityScore << ","
                    << "\"quality_level\":\"" << fusion.level << "\","
                    << "\"active_ratio\":" << fusion.activeRatio << ","
                    << "\"ebpf_correction\":" << fusion.ebpfCorrection << ","
                    << "\"bytes_per_sec\":" << fusion.bytesPerSec << ","
                    << "\"max_gap_ms\":" << fusion.maxGapMs << ","
                    << "\"ebpf_available\":" << (fusion.ebpfAvailable ? "true" : "false") << ","
                    << "\"diagnostic\":\"" << fusion.diagnostic << "\""
                    << "}";
                jsonResult = oss.str();
            }
        }
    }

    DBusMessageIter args;
    dbus_message_iter_init_append(reply, &args);
    const char* s = jsonResult.c_str();
    dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &s);
    dbus_connection_send(conn, reply, nullptr);
    dbus_connection_flush(conn);
    dbus_message_unref(reply);
    return true;
}

/**
 * @brief DBus 方法实现：GetCoexistenceConflict —— 返回 Wi-Fi 与蓝牙 2.4GHz 冲突诊断 JSON
 */
bool DbusService::handleGetCoexistenceConflict(DBusConnection* conn, DBusMessage* msg) {
    LOG_INFO(LogModule::DBUS, "handleGetCoexistenceConflict called");
    DBusMessage* reply = dbus_message_new_method_return(msg);
    if (!reply) return false;

    std::string jsonResult;
    if (ctx_) {
        std::lock_guard<std::mutex> lock(ctx_->conflict_mutex);
        jsonResult = ctx_->latest_conflict_json;
    } else {
        jsonResult = "{\"detected\":false,\"confidence\":0.0,\"reason\":\"Server context not available\"}";
    }

    DBusMessageIter args;
    dbus_message_iter_init_append(reply, &args);
    const char* s = jsonResult.c_str();
    dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &s);
    dbus_connection_send(conn, reply, nullptr);
    dbus_connection_flush(conn);
    dbus_message_unref(reply);
    return true;
}

/**
 * @brief DBus 方法实现：GetNetworkExperience —— 返回权威评估快照（schema v2）
 *
 * **只读 snapshot，绝不重新 evaluate**（W2 单一事实源）：
 * quality 线程是唯一 evaluator 执行点；本方法只序列化 stabilizer 后的
 * 最终结论。实测此前三条评估路径（quality/history/HealthCheck）各自拉
 * 不同 metrics、结论可能不一致 —— 该 bug 由本方法杜绝。
 *
 * 生命周期边界：
 *   - 尚无第一份 snapshot → 显式 UNKNOWN(no_assessment_yet)，不返回空对象
 *   - config_generation / network_epoch 与当前不符 → 旧 snapshot 失效，
 *     同样返回 UNKNOWN(stale_assessment)，等下一轮评估
 */
bool DbusService::handleGetNetworkExperience(DBusConnection* conn, DBusMessage* msg) {
    LOG_INFO(LogModule::DBUS, "handleGetNetworkExperience called (W2 read-only snapshot)");

    std::string jsonResult;
    auto snap = ctx_ ? ctx_->assessment_store.latest() : nullptr;
    bool stale = false;
    if (snap) {
        // 配置代/网络代与当前不符 → 旧 snapshot 失效
        const uint64_t cur_epoch = ctx_->dns_tracker ? ctx_->dns_tracker->currentBindingEpoch()
                                                     : ctx_->dns_binding_epoch.load();
        if (!weaknet::AssessmentSnapshotStore::isCurrent(*snap,
                ctx_->cfg.config_generation.load(), cur_epoch)) {
            stale = true;
        }
    }

    if (!snap) {
        jsonResult = "{\"schema_version\":2,\"state\":\"UNKNOWN\","
                     "\"coverage\":\"NONE\",\"reason\":\"no_assessment_yet\"}";
    } else if (stale) {
        jsonResult = "{\"schema_version\":2,\"state\":\"UNKNOWN\","
                     "\"coverage\":\"NONE\",\"reason\":\"stale_assessment\","
                     "\"note\":\"waiting for next evaluation with current config/epoch\"}";
    } else {
        jsonResult = weaknet::LegacyAdapter::toExperienceJsonV2(snap->experience);
    }

    DBusMessage* reply = dbus_message_new_method_return(msg);
    if (!reply) return false;
    DBusMessageIter args;
    dbus_message_iter_init_append(reply, &args);
    const char* s = jsonResult.c_str();
    if (!dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &s)) {
        dbus_message_unref(reply);
        return false;
    }
    dbus_connection_send(conn, reply, nullptr);
    dbus_connection_flush(conn);
    dbus_message_unref(reply);
    return true;
}


// ====================================================================
// eBPF 监控数据 D-Bus 方法
// ====================================================================
// 以下 5 个方法结构高度相似：获取对应 monitor → 检查 isAvailable() →
// 用 "key:value|key:value" 格式拼接 → 返回单字符串。
// 区别仅在数据源和字段集合。

/**
 * @brief DBus 方法实现：GetDnsStats —— 返回 DNS 监控统计
 * 字段: totalQueries | totalResponses | totalTimeouts | totalErrors | avgLatencyMs | maxLatencyMs | timeoutRate
 */
bool DbusService::handleGetDnsStats(DBusConnection* conn, DBusMessage* msg) {
    LOG_INFO(LogModule::DBUS, "handleGetDnsStats called");
    DBusMessage* reply = dbus_message_new_method_return(msg);
    if (!reply) return false;

    std::string result;
    DnsMonitor* monitor = ctx_ ? ctx_->dns_monitor : nullptr;
    if (monitor && monitor->isAvailable()) {
        auto stats = monitor->getStats();
        result = "totalQueries:" + std::to_string(stats.totalQueries)
            + "|totalResponses:" + std::to_string(stats.totalResponses)
            + "|totalTimeouts:" + std::to_string(stats.totalTimeouts)
            + "|totalErrors:" + std::to_string(stats.totalErrors)
            + "|avgLatencyMs:" + std::to_string(stats.avgLatencyMs)
            + "|maxLatencyMs:" + std::to_string(stats.maxLatencyMs)
            + "|timeoutRate:" + std::to_string(stats.timeoutRate());
    } else {
        result = "DNS monitor not available";
    }

    DBusMessageIter args;
    dbus_message_iter_init_append(reply, &args);
    const char* s = result.c_str();
    dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &s);
    dbus_connection_send(conn, reply, nullptr);
    dbus_connection_flush(conn);
    dbus_message_unref(reply);
    return true;
}

/**
 * @brief DBus 方法实现：GetWifiLossStats —— 返回 Wi-Fi 丢包监控统计
 *
 * 多网卡场景：按 ifindex 分隔每段 "ifindex:N|rxPkts:...|txPkts:...|txDrops:...|txLossRate:...%|"
 */
bool DbusService::handleGetWifiLossStats(DBusConnection* conn, DBusMessage* msg) {
    LOG_INFO(LogModule::DBUS, "handleGetWifiLossStats called");
    DBusMessage* reply = dbus_message_new_method_return(msg);
    if (!reply) return false;

    std::string result;
    WifiPacketLossMonitor* monitor = ctx_ ? ctx_->wifi_loss_monitor : nullptr;
    if (monitor && monitor->isAvailable()) {
        auto stats = monitor->getStats();
        for (auto& [ifindex, s] : stats) {
            result += "ifindex:" + std::to_string(ifindex)
                + " rxPkts:" + std::to_string(s.rxPkts)
                + " txPkts:" + std::to_string(s.txPkts)
                + " txDrops:" + std::to_string(s.txDrops)
                + " txLossRate:" + std::to_string(s.txLossRate()) + "%"
                + "|";
        }
        if (result.empty()) result = "No interface stats available";
    } else {
        result = "Wi-Fi loss monitor not available";
    }

    DBusMessageIter args;
    dbus_message_iter_init_append(reply, &args);
    const char* s = result.c_str();
    dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &s);
    dbus_connection_send(conn, reply, nullptr);
    dbus_connection_flush(conn);
    dbus_message_unref(reply);
    return true;
}

/**
 * @brief DBus 方法实现：GetHttpLatencyStats —— 返回 HTTP 延迟监控统计
 * 字段: totalTxns | p50Ms | p95Ms | p99Ms | maxMs | analysis
 */
bool DbusService::handleGetHttpLatencyStats(DBusConnection* conn, DBusMessage* msg) {
    LOG_INFO(LogModule::DBUS, "handleGetHttpLatencyStats called");
    DBusMessage* reply = dbus_message_new_method_return(msg);
    if (!reply) return false;

    std::string result;
    HttpLatencyMonitor* monitor = ctx_ ? ctx_->http_latency_monitor : nullptr;
    if (monitor && monitor->isAvailable()) {
        auto stats = monitor->getGlobalStats();
        result = "totalTxns:" + std::to_string(stats.totalTxns)
            + "|p50Ms:" + std::to_string(stats.p50Ns / 1000000)
            + "|p95Ms:" + std::to_string(stats.p95Ns / 1000000)
            + "|p99Ms:" + std::to_string(stats.p99Ns / 1000000)
            + "|maxMs:" + std::to_string(stats.maxNs / 1000000)
            + "|analysis:" + stats.analysis;
    } else {
        result = "HTTP latency monitor not available";
    }

    DBusMessageIter args;
    dbus_message_iter_init_append(reply, &args);
    const char* s = result.c_str();
    dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &s);
    dbus_connection_send(conn, reply, nullptr);
    dbus_connection_flush(conn);
    dbus_message_unref(reply);
    return true;
}

/**
 * @brief DBus 方法实现：GetProcessProfiling —— 返回进程级网络 profiling 数据
 *
 * 输出分两段：Top Bandwidth（按 txBytes 排序 top 5）和 Top Retransmit（按重传次数排序 top 5）。
 * 每条格式: "pid:N comm:xxx txBytes:... txPackets:... retrans:...|"
 */
bool DbusService::handleGetProcessProfiling(DBusConnection* conn, DBusMessage* msg) {
    LOG_INFO(LogModule::DBUS, "handleGetProcessProfiling called");
    DBusMessage* reply = dbus_message_new_method_return(msg);
    if (!reply) return false;

    std::string result;
    ProcessNetProfiler* monitor = ctx_ ? ctx_->process_net_profiler : nullptr;
    if (monitor && monitor->isAvailable()) {
        result += "=== Top Bandwidth ===|";
        auto topBw = monitor->getTopBandwidth(5);
        for (auto& p : topBw) {
            result += "pid:" + std::to_string(p.pid)
                + " comm:" + p.comm
                + " txBytes:" + std::to_string(p.txBytes)
                + " txPackets:" + std::to_string(p.txPackets)
                + " retrans:" + std::to_string(p.retransCount)
                + "|";
        }
        result += "=== Top Retransmit ===|";
        auto topRetrans = monitor->getTopRetransmit(5);
        for (auto& p : topRetrans) {
            result += "pid:" + std::to_string(p.pid)
                + " comm:" + p.comm
                + " txBytes:" + std::to_string(p.txBytes)
                + " retrans:" + std::to_string(p.retransCount)
                + "|";
        }
    } else {
        result = "Process net profiler not available";
    }

    DBusMessageIter args;
    dbus_message_iter_init_append(reply, &args);
    const char* s = result.c_str();
    dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &s);
    dbus_connection_send(conn, reply, nullptr);
    dbus_connection_flush(conn);
    dbus_message_unref(reply);
    return true;
}

/**
 * @brief DBus 方法实现：GetSkbDropStats —— 查询 Socket/skb 丢包原因精确归因统计（JSON）
 */
bool DbusService::handleGetSkbDropStats(DBusConnection* conn, DBusMessage* msg) {
    LOG_INFO(LogModule::DBUS, "handleGetSkbDropStats called");
    DBusMessage* reply = dbus_message_new_method_return(msg);
    if (!reply) {
        LOG_ERROR(LogModule::DBUS, "handleGetSkbDropStats: failed to create reply");
        return false;
    }

    std::ostringstream json;
    if (ctx_ && ctx_->skb_drop_monitor) {
        auto summary = ctx_->skb_drop_monitor->getDropStats();
        json << "{\"total_drops\":" << summary.totalDrops
             << ",\"top_reasons\":[";
        for (size_t i = 0; i < summary.topReasons.size(); ++i) {
            if (i > 0) json << ",";
            const auto& item = summary.topReasons[i];
            json << "{\"reason_code\":" << item.reasonCode
                 << ",\"reason_name\":\"" << weaknet_utils::escapeJsonString(item.reasonName)
                 << "\",\"description\":\"" << weaknet_utils::escapeJsonString(item.humanDesc)
                 << "\",\"protocol\":\"" << weaknet_utils::escapeJsonString(item.protocol)
                 << "\",\"count\":" << item.count
                 << ",\"last_timestamp_ns\":" << item.lastTimestampNs << "}";
        }
        json << "]}";
    } else {
        json << "{\"total_drops\":0,\"top_reasons\":[],\"error\":\"skb_drop monitor unavailable\"}";
    }

    std::string result = json.str();
    DBusMessageIter args;
    dbus_message_iter_init_append(reply, &args);
    const char* s = result.c_str();
    dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &s);
    dbus_connection_send(conn, reply, nullptr);
    dbus_connection_flush(conn);
    dbus_message_unref(reply);
    return true;
}

/**
 * @brief DBus 方法实现：GetEbpfMonitorHealth —— 返回所有 eBPF monitor 健康状态（JSON）
 * @return JSON 字符串,结构: {"monitors":[{name,state,available,healthy,map_reads,map_read_errors,samples,average_read_time_us,status}, ...]}
 *
 * 遍历 8 个实现了 IEbpfMonitor 接口的组件（DNS/Wi-Fi 丢包/HTTP 延迟/进程 profiling/
 * TCP 重传/TCP 连接统计/Socket 丢包/蓝牙音频），每个采集 health() 和 metrics()。
 * 蓝牙音频分析器为可选项：对象未创建时输出一份 "uninitialized" 占位条目，
 * 保证 JSON 始终包含全部项且不出现空指针解引用。
 */
bool DbusService::handleGetEbpfMonitorHealth(DBusConnection* conn, DBusMessage* msg) {
    LOG_INFO(LogModule::DBUS, "handleGetEbpfMonitorHealth called");
    if (!ctx_) {
        DBusMessage* error = dbus_message_new_error(msg, "com.example.WeakNet.Error", "monitor context unavailable");
        if (error) { dbus_connection_send(conn, error, nullptr); dbus_message_unref(error); }
        return false;
    }

    // 每个 monitor 独立输出；停止或初始化失败的对象以 unavailable 占位。
    const auto monitorState = [this](const char* name) {
        MonitorStatus status;
        return ctx_->monitor_manager && ctx_->monitor_manager->status(name, &status)
            ? std::string(monitorStateName(status.state)) : std::string("unavailable");
    };
    const IEbpfMonitor* bt_audio = nullptr;
    if (ctx_->bt_monitor) {
        bt_audio = ctx_->bt_monitor->audioAnalyzer();
    }

    const std::vector<std::pair<const char*, const IEbpfMonitor*>> monitors = {
        {"DnsMonitor", static_cast<const IEbpfMonitor*>(ctx_->dns_monitor)},
        {"WifiPacketLossMonitor", static_cast<const IEbpfMonitor*>(ctx_->wifi_loss_monitor)},
        {"HttpLatencyMonitor", static_cast<const IEbpfMonitor*>(ctx_->http_latency_monitor)},
        {"ProcessNetProfiler", static_cast<const IEbpfMonitor*>(ctx_->process_net_profiler)},
        {"TcpRetransMonitor", static_cast<const IEbpfMonitor*>(ctx_->tcp_retrans_monitor)},
        {"TcpConnMonitor", static_cast<const IEbpfMonitor*>(ctx_->tcp_conn_monitor)},
        {"SkbDropMonitor", static_cast<const IEbpfMonitor*>(ctx_->skb_drop_monitor)},
        {"BtAudioAnalyzer", bt_audio}
    };

    // 手工拼接 JSON：项目不依赖 JSON 库，字段名和字符串值都要做 JSON 转义
    std::ostringstream json;
    json << "{\"monitors\":[";
    for (size_t i = 0; i < monitors.size(); ++i) {
        if (i > 0) json << ",";
        if (!monitors[i].second) {
            json << "{\"name\":\"" << weaknet_utils::escapeJsonString(monitors[i].first)
                 << "\",\"state\":\"" << monitorState(monitors[i].first)
                 << "\",\"available\":false,\"healthy\":false,\"last_successful_sample_ns\":0,\"consecutive_errors\":0"
                 << ",\"total_errors\":0,\"attached_probes\":0,\"map_reads\":0,\"map_read_errors\":0"
                 << ",\"samples\":0,\"total_read_time_us\":0,\"average_read_time_us\":0"
                 << ",\"last_error\":\"monitor not running\",\"status\":\"unavailable\"}";
            continue;
        }
        const auto health = monitors[i].second->health();
        const auto metrics = monitors[i].second->metrics();
        json << "{\"name\":\"" << weaknet_utils::escapeJsonString(health.name)
             << "\",\"state\":\"" << ebpfMonitorStateName(health.state)
             << "\",\"available\":" << (health.available ? "true" : "false")
             << ",\"healthy\":" << (health.healthy ? "true" : "false")
             << ",\"last_successful_sample_ns\":" << health.lastSuccessfulSampleNs
             << ",\"consecutive_errors\":" << health.consecutiveErrors
             << ",\"total_errors\":" << health.totalErrors
             << ",\"attached_probes\":" << metrics.attachedProbes
             << ",\"map_reads\":" << metrics.mapReads
             << ",\"map_read_errors\":" << metrics.mapReadErrors
             << ",\"samples\":" << metrics.samples
             << ",\"total_read_time_us\":" << metrics.totalReadTimeUs
             << ",\"average_read_time_us\":" << metrics.averageReadTimeUs
             << ",\"last_error\":\"" << weaknet_utils::escapeJsonString(metrics.lastError)
             << "\",\"status\":\"" << weaknet_utils::escapeJsonString(health.status)
             << "\"}";
    }
    json << "]}";

    DBusMessage* reply = dbus_message_new_method_return(msg);
    if (!reply) return false;
    DBusMessageIter args;
    dbus_message_iter_init_append(reply, &args);
    const std::string result = json.str();
    const char* value = result.c_str();
    dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &value);
    bool ok = dbus_connection_send(conn, reply, nullptr);
    dbus_connection_flush(conn);
    dbus_message_unref(reply);
    return ok;
}

/**
 * @brief DBus 方法实现：GetHistory —— 从数据库查询历史记录
 * @param conn DBus 连接
 * @param msg 接收到的方法调用消息（参数：interface[可选]、start[可选]、end[可选]、limit[可选，默认 100]）
 * @return true 回复发送成功；DB 返回值是 JSON 字符串（或错误 JSON）
 *
 * 参数解析采用"逐次 dbus_message_iter_next"风格：先按约定顺序迭代，缺省的用默认值填充。
 * 任何参数类型都没有严格强制——缺失或类型不符就跳过，用默认值。
 */
bool DbusService::handleGetHistory(DBusConnection* conn, DBusMessage* msg) {
    LOG_INFO(LogModule::DBUS, "handleGetHistory called");

    // 解析收到的消息必须用 dbus_message_iter_init（读迭代器）；
    // init_append 是写迭代器，对入站消息使用会触发 libdbus 断言/未定义行为。
    // init 返回 FALSE 表示消息无参数，全部走缺省值。
    std::string iface_filter, start_time, end_time;
    int32_t limit = 100;

    DBusMessageIter args;
    const bool hasArgs = dbus_message_iter_init(msg, &args) == TRUE;
    if (hasArgs) {
    // 参数 1: interface (string)，可缺省
    if (dbus_message_iter_get_arg_type(&args) == DBUS_TYPE_STRING) {
        const char* val = nullptr;
        dbus_message_iter_get_basic(&args, &val);
        if (val) iface_filter = val;
    }
    if (dbus_message_iter_next(&args)) {
        // 参数 2: start (string)
        if (dbus_message_iter_get_arg_type(&args) == DBUS_TYPE_STRING) {
            const char* val = nullptr;
            dbus_message_iter_get_basic(&args, &val);
            if (val) start_time = val;
        }
    }
    if (dbus_message_iter_next(&args)) {
        // 参数 3: end (string)
        if (dbus_message_iter_get_arg_type(&args) == DBUS_TYPE_STRING) {
            const char* val = nullptr;
            dbus_message_iter_get_basic(&args, &val);
            if (val) end_time = val;
        }
    }
    if (dbus_message_iter_next(&args)) {
        // 参数 4: limit (int32)
        if (dbus_message_iter_get_arg_type(&args) == DBUS_TYPE_INT32) {
            dbus_message_iter_get_basic(&args, &limit);
        }
    }
    }  // hasArgs

    // 钳制 limit：该接口对本地任意用户开放（见 com.example.WeakNet.conf 的
    // default 上下文），而 SQLite 把负 LIMIT 解释为"无上限"。若把 -1 直接
    // 透传，queryHistory 会把整张历史表拼成一个 std::ostringstream 并作为
    // 单条 D-Bus 字符串回发，足以耗尽内存或触发 128MB 消息上限。
    // 0 同样危险（静默返回空集），故一并归一为下限 1。
    constexpr int32_t kMaxHistoryLimit = 10000;
    if (limit <= 0) {
        LOG_WARNING(LogModule::DBUS, "GetHistory limit=" << limit
                    << " is non-positive; clamped to 1");
        limit = 1;
    } else if (limit > kMaxHistoryLimit) {
        LOG_WARNING(LogModule::DBUS, "GetHistory limit=" << limit
                    << " exceeds cap; clamped to " << kMaxHistoryLimit);
        limit = kMaxHistoryLimit;
    }

    std::string result = "[]";
    if (ctx_ && ctx_->db_mgr && ctx_->db_mgr->isOpen()) {
        result = ctx_->db_mgr->queryHistory(iface_filter, start_time, end_time, limit);
    } else {
        result = "{\"error\":\"database not available\"}";
    }

    DBusMessage* reply = dbus_message_new_method_return(msg);
    if (!reply) return false;

    DBusMessageIter reply_args;
    dbus_message_iter_init_append(reply, &reply_args);
    const char* s = result.c_str();
    dbus_message_iter_append_basic(&reply_args, DBUS_TYPE_STRING, &s);
    dbus_connection_send(conn, reply, nullptr);
    dbus_connection_flush(conn);
    dbus_message_unref(reply);
    return true;
}

// ============================================================================
// 运行时配置方法
// ============================================================================

bool DbusService::handleSetMonitorParam(DBusConnection* conn, DBusMessage* msg) {
    LOG_INFO(LogModule::DBUS, "handleSetMonitorParam called");

    // 运行时调参能改变设备行为（例如 active_probe.targets 决定它主动连接谁），
    // 因此与 ExecuteAction 同级：仅 root 可写。
    if (!requireRootCaller(conn, msg, "SetMonitorParam")) return false;

    DBusError err;
    dbus_error_init(&err);
    const char* key = nullptr;
    const char* value = nullptr;

    if (!dbus_message_get_args(msg, &err, DBUS_TYPE_STRING, &key,
                               DBUS_TYPE_STRING, &value, DBUS_TYPE_INVALID)) {
        LOG_ERROR(LogModule::DBUS, "SetMonitorParam arg error: " << err.message);
        dbus_error_free(&err);
        DBusMessage* reply = dbus_message_new_error(msg, "com.example.WeakNet.Error", "Invalid arguments (expect: string key, string value)");
        dbus_connection_send(conn, reply, nullptr);
        dbus_message_unref(reply);
        return false;
    }
    if (!key || !value) {
        DBusMessage* reply = dbus_message_new_error(msg, "com.example.WeakNet.Error", "Null key or value");
        dbus_connection_send(conn, reply, nullptr);
        dbus_message_unref(reply);
        return false;
    }

    LOG_INFO(LogModule::DBUS, "SetMonitorParam: " << key << " = " << value);
    std::string cfg_err;

    // TRIAL 中本地调参会让回滚基线失真（trial 期间改过的 key，回滚时
    // prior_values 已含云端的 trial 前值；本地再写一遍，等回滚来时被
    // 覆盖成 trial 前 —— 等于把运维在 trial 期间的合法改动吞掉）。
    // 因此 TRIAL 中显式拒绝本地写，等 confirm/rollback 后再放开。
    if (ctx_->config_txn &&
        ctx_->config_txn->state() == weaknet_dbus::ConfigState::TRIAL) {
        cfg_err = "trial_in_progress";
        LOG_ERROR(LogModule::DBUS, "SetMonitorParam rejected: " << cfg_err);
        DBusMessage* reply = dbus_message_new_error(msg, "com.example.WeakNet.Error", cfg_err.c_str());
        dbus_connection_send(conn, reply, nullptr);
        dbus_message_unref(reply);
        return false;
    }

    // ctx_->cfg 是线程安全配置，setMonitorParam 内部对目标字段做校验+原子写入
    if (!setMonitorParam(&ctx_->cfg, key, value, &cfg_err)) {
        LOG_ERROR(LogModule::DBUS, "SetMonitorParam rejected: " << cfg_err);
        DBusMessage* reply = dbus_message_new_error(msg, "com.example.WeakNet.Error", cfg_err.c_str());
        dbus_connection_send(conn, reply, nullptr);
        dbus_message_unref(reply);
        return false;
    }

    LOG_INFO(LogModule::DBUS, "SetMonitorParam applied: " << key << " = " << value);

    // 返回成功
    DBusMessage* reply = dbus_message_new_method_return(msg);
    if (!reply) return false;

    const char* status = "ok";
    DBusMessageIter reply_args;
    dbus_message_iter_init_append(reply, &reply_args);
    dbus_message_iter_append_basic(&reply_args, DBUS_TYPE_STRING, &status);
    dbus_connection_send(conn, reply, nullptr);
    dbus_connection_flush(conn);
    dbus_message_unref(reply);
    return true;
}

bool DbusService::handleGetMonitorParam(DBusConnection* conn, DBusMessage* msg) {
    LOG_INFO(LogModule::DBUS, "handleGetMonitorParam called");

    DBusError err;
    dbus_error_init(&err);
    const char* monitor = nullptr;
    if (!dbus_message_get_args(msg, &err, DBUS_TYPE_STRING, &monitor, DBUS_TYPE_INVALID)) {
        LOG_ERROR(LogModule::DBUS, "GetMonitorParam arg error: " << err.message);
        dbus_error_free(&err);
        DBusMessage* reply = dbus_message_new_error(msg, "com.example.WeakNet.Error", "Invalid arguments (expect: string monitor)");
        dbus_connection_send(conn, reply, nullptr);
        dbus_message_unref(reply);
        return false;
    }
    if (!monitor) {
        DBusMessage* reply = dbus_message_new_error(msg, "com.example.WeakNet.Error", "Null monitor name");
        dbus_connection_send(conn, reply, nullptr);
        dbus_message_unref(reply);
        return false;
    }

    LOG_INFO(LogModule::DBUS, "GetMonitorParam: " << monitor);
    std::string cfg_err;
    std::string json = serializeMonitorJson(ctx_->cfg, monitor, &cfg_err);
    if (json.empty()) {
        LOG_ERROR(LogModule::DBUS, "GetMonitorParam unknown monitor: " << monitor << " (" << cfg_err << ")");
        DBusMessage* reply = dbus_message_new_error(msg, "com.example.WeakNet.Error", cfg_err.c_str());
        dbus_connection_send(conn, reply, nullptr);
        dbus_message_unref(reply);
        return false;
    }

    DBusMessage* reply = dbus_message_new_method_return(msg);
    if (!reply) return false;

    const char* s = json.c_str();
    DBusMessageIter reply_args;
    dbus_message_iter_init_append(reply, &reply_args);
    dbus_message_iter_append_basic(&reply_args, DBUS_TYPE_STRING, &s);
    dbus_connection_send(conn, reply, nullptr);
    dbus_connection_flush(conn);
    dbus_message_unref(reply);
    return true;
}

// ============================================================================
// 监控器生命周期方法
// ============================================================================

namespace {

std::string monitorStatusJson(const MonitorStatus& status) {
    return std::string("{\"name\":\"") + weaknet_utils::escapeJsonString(status.name)
        + "\",\"state\":\"" + monitorStateName(status.state)
        + "\",\"desired_enabled\":" + (status.desired_enabled ? "true" : "false")
        + ",\"generation\":" + std::to_string(status.generation)
        + ",\"changed_at\":\"" + weaknet_utils::escapeJsonString(status.changed_at)
        + "\",\"error\":\"" + weaknet_utils::escapeJsonString(status.error) + "\"}";
}

bool monitorNameArg(DBusMessage* msg, const char** name, DBusError* err) {
    return dbus_message_get_args(msg, err, DBUS_TYPE_STRING, name, DBUS_TYPE_INVALID)
        && name && *name;
}

}

bool DbusService::handleListMonitors(DBusConnection* conn, DBusMessage* msg) {
    if (!ctx_ || !ctx_->monitor_manager) return false;
    std::string result = "[";
    bool first = true;
    for (const auto& status : ctx_->monitor_manager->list()) {
        if (!first) result += ",";
        first = false;
        result += monitorStatusJson(status);
    }
    result += "]";
    DBusMessage* reply = dbus_message_new_method_return(msg);
    if (!reply) return false;
    DBusMessageIter iter;
    dbus_message_iter_init_append(reply, &iter);
    const char* value = result.c_str();
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &value);
    dbus_connection_send(conn, reply, nullptr);
    dbus_connection_flush(conn);
    dbus_message_unref(reply);
    return true;
}

bool DbusService::handleGetMonitorStatus(DBusConnection* conn, DBusMessage* msg) {
    DBusError err;
    dbus_error_init(&err);
    const char* name = nullptr;
    if (!monitorNameArg(msg, &name, &err)) {
        const char* text = dbus_error_is_set(&err) ? err.message : "missing monitor name";
        DBusMessage* reply = dbus_message_new_error(msg, "com.example.WeakNet.Error", text);
        if (reply) { dbus_connection_send(conn, reply, nullptr); dbus_message_unref(reply); }
        dbus_error_free(&err);
        return false;
    }
    MonitorStatus status;
    if (!ctx_ || !ctx_->monitor_manager || !ctx_->monitor_manager->status(name, &status)) {
        DBusMessage* reply = dbus_message_new_error(msg, "com.example.WeakNet.Error", "unknown monitor");
        if (reply) { dbus_connection_send(conn, reply, nullptr); dbus_message_unref(reply); }
        return false;
    }
    std::string result = monitorStatusJson(status);
    DBusMessage* reply = dbus_message_new_method_return(msg);
    if (!reply) return false;
    DBusMessageIter iter;
    dbus_message_iter_init_append(reply, &iter);
    const char* value = result.c_str();
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &value);
    dbus_connection_send(conn, reply, nullptr);
    dbus_connection_flush(conn);
    dbus_message_unref(reply);
    return true;
}

bool DbusService::handleEnableMonitor(DBusConnection* conn, DBusMessage* msg) {
    return handleMonitorOperation(conn, msg, "enable");
}

bool DbusService::handleDisableMonitor(DBusConnection* conn, DBusMessage* msg) {
    return handleMonitorOperation(conn, msg, "disable");
}

bool DbusService::handleRestartMonitor(DBusConnection* conn, DBusMessage* msg) {
    return handleMonitorOperation(conn, msg, "restart");
}


// The lifecycle handlers share the same argument validation and reply contract.
bool DbusService::handleMonitorOperation(DBusConnection* conn, DBusMessage* msg,
                                         const char* operation) {
    // enable/disable/restart 会改变评估管线的运行态（例如停掉 quality 即静默
    // 关闭整套评估），与运行时调参同级：仅 root 可写。
    if (!requireRootCaller(conn, msg, operation)) return false;

    DBusError err;
    dbus_error_init(&err);
    const char* name = nullptr;
    if (!monitorNameArg(msg, &name, &err)) {
        const char* text = dbus_error_is_set(&err) ? err.message : "missing monitor name";
        DBusMessage* reply = dbus_message_new_error(msg, "com.example.WeakNet.Error", text);
        if (reply) { dbus_connection_send(conn, reply, nullptr); dbus_message_unref(reply); }
        dbus_error_free(&err);
        return false;
    }
    if (!ctx_ || !ctx_->monitor_manager) {
        DBusMessage* reply = dbus_message_new_error(msg, "com.example.WeakNet.Error", "monitor manager unavailable");
        if (reply) { dbus_connection_send(conn, reply, nullptr); dbus_message_unref(reply); }
        return false;
    }
    std::string error;
    bool ok = false;
    if (std::string(operation) == "enable") ok = ctx_->monitor_manager->enable(name, &error);
    else if (std::string(operation) == "disable") ok = ctx_->monitor_manager->disable(name, &error);
    else ok = ctx_->monitor_manager->restart(name, &error);
    if (!ok) {
        DBusMessage* reply = dbus_message_new_error(msg, "com.example.WeakNet.Error", error.c_str());
        if (reply) { dbus_connection_send(conn, reply, nullptr); dbus_message_unref(reply); }
        return false;
    }
    MonitorStatus status;
    ctx_->monitor_manager->status(name, &status);
    std::string result = monitorStatusJson(status);
    DBusMessage* reply = dbus_message_new_method_return(msg);
    if (!reply) return false;
    DBusMessageIter iter;
    dbus_message_iter_init_append(reply, &iter);
    const char* value = result.c_str();
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &value);
    dbus_connection_send(conn, reply, nullptr);
    dbus_connection_flush(conn);
    dbus_message_unref(reply);
    return true;
}

bool DbusService::handleSaveMonitorOverrides(DBusConnection* conn, DBusMessage* msg) {
    // 持久化运行时启停状态，影响后续启动行为：仅 root 可写。
    if (!requireRootCaller(conn, msg, "SaveMonitorOverrides")) return false;

    std::string error;
    if (!ctx_ || !ctx_->monitor_manager || !ctx_->monitor_manager->saveOverrides(&error)) {
        DBusMessage* reply = dbus_message_new_error(
            msg, "com.example.WeakNet.Error",
            error.empty() ? "failed to save monitor overrides" : error.c_str());
        if (reply) { dbus_connection_send(conn, reply, nullptr); dbus_message_unref(reply); }
        return false;
    }
    const char* result = "ok";
    DBusMessage* reply = dbus_message_new_method_return(msg);
    if (!reply) return false;
    DBusMessageIter iter;
    dbus_message_iter_init_append(reply, &iter);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &result);
    dbus_connection_send(conn, reply, nullptr);
    dbus_connection_flush(conn);
    dbus_message_unref(reply);
    return true;
}

}  // namespace weaknet_dbus
