/**
 * @file edge_wireless_uplink_exporter.cpp
 * @brief 无线事实上行器实现（Phase 4a）
 *
 * 关键实现点（与头文件约束一一对应）：
 *   1. buildBody() 是**唯一**序列化入口：签名与发送共用同一段字节；
 *   2. 游标只在 HTTP 2xx 后前移；失败原地重试（服务端按 id 幂等）；
 *   3. 采集顺序固定（事件按 ts、事故按 last_event_ms、基线按 updated_at），
 *      让"哪些事实已发出"可确定地推进；
 *   4. 环境证据当前恒为 available=false —— 板端尚未采集结构化环境快照，
 *      不用 0/空值冒充"观测过且正常"。
 */

#include "edge_wireless_uplink_exporter.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#ifdef WEAKNET_HAVE_CURL
#include <curl/curl.h>
#endif

#include "database_manager.hpp"
#include "logger.hpp"
#include "utils/json_escape.hpp"
#include "weaknet_config.hpp"

#ifdef WEAKNET_HAVE_TLS
#include <openssl/evp.h>
#include <openssl/pem.h>
#endif

namespace weaknet {

namespace {

constexpr uint32_t kUplinkIntervalMsDefault = 10000;
constexpr uint32_t kUplinkIntervalMsMin = 1000;
constexpr uint32_t kUplinkIntervalMsMax = 3600000;

/// 游标只跟踪事件水位：事故与基线都按"水位之后"近似采集。
/// 事故按 last_event_ms 过滤、基线按 updated_at 过滤，它们的时间轴与事件
/// 同域（墙钟毫秒），因此共用一个水位即可；重复发送由云端幂等吸收。
std::string jsonStr(const std::string& raw) {
    return "\"" + weaknet_utils::escapeJsonString(raw) + "\"";
}

std::string jsonNullableInt(const std::optional<int16_t>& v) {
    return v.has_value() ? std::to_string(static_cast<int>(*v)) : std::string("null");
}

/// 里程碑：把一条事故的证据回链拼成 JSON 数组
std::string jsonStringArray(const std::vector<std::string>& items) {
    std::ostringstream oss;
    oss << "[";
    bool first = true;
    for (const auto& item : items) {
        if (!first) oss << ",";
        first = false;
        oss << jsonStr(item);
    }
    oss << "]";
    return oss.str();
}

#ifdef WEAKNET_HAVE_CURL
size_t writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    const size_t total = size * nmemb;
    static_cast<std::string*>(userdata)->append(ptr, total);
    return total;
}
#endif

std::string readUintField(const std::string& text, const std::string& key) {
    const std::string needle = key + "=";
    size_t pos = text.find(needle);
    if (pos == std::string::npos) return "";
    pos += needle.size();
    size_t end = text.find('\n', pos);
    if (end == std::string::npos) end = text.size();
    return text.substr(pos, end - pos);
}

}  // namespace

// ============================================================================
// 序列化（唯一入口）
// ============================================================================

std::string EdgeWirelessUplinkExporter::buildBody(const WirelessUplinkPayload& payload,
                                                  std::string* error) {
    if (payload.device_id.empty()) {
        if (error) *error = "device_id 为空";
        return {};
    }
    if (payload.events.size() > EDGE_WIRELESS_MAX_EVENTS_PER_BATCH ||
        payload.incidents.size() > EDGE_WIRELESS_MAX_INCIDENTS_PER_BATCH ||
        payload.baselines.size() > EDGE_WIRELESS_MAX_BASELINES_PER_BATCH) {
        if (error) *error = "单批事实数超出契约上限";
        return {};
    }
    // 云端契约要求至少一组非空：空批不发送（避免无意义的 422）
    const bool env_present = payload.env_to_ms > payload.env_from_ms;
    if (payload.events.empty() && payload.incidents.empty() && payload.baselines.empty() &&
        !env_present) {
        if (error) *error = "空批";
        return {};
    }

    std::ostringstream body;
    body << "{\"schema_version\":\"network.edge.wireless-events.v1\",";
    body << "\"device_id\":" << jsonStr(payload.device_id) << ",";
    body << "\"watermark_ms\":" << payload.watermark_ms << ",";

    body << "\"events\":[";
    for (size_t i = 0; i < payload.events.size(); ++i) {
        const auto& ev = payload.events[i];
        if (i) body << ",";
        body << "{"
             << "\"event_id\":" << jsonStr(ev.event_id) << ","
             << "\"ts_ms\":" << ev.timestamp_ms << ","
             << "\"site_id\":" << jsonStr(ev.site_id) << ","
             << "\"gateway_id\":" << jsonStr(ev.gateway_id) << ","
             << "\"protocol\":" << jsonStr(toString(ev.protocol)) << ","
             << "\"device_address\":" << jsonStr(ev.device_address) << ","
             << "\"address_type\":" << jsonStr(toString(ev.address_type)) << ","
             << "\"hci_index\":" << ev.hci_index << ","
             << "\"event_type\":" << jsonStr(toString(ev.event_type)) << ","
             << "\"rssi_at_event_dbm\":"
             << (ev.rssi_at_event_dbm.has_value() ? std::to_string(*ev.rssi_at_event_dbm)
                                                  : std::string("null"))
             << ","
             << "\"raw_reason_code\":" << static_cast<int>(ev.raw_reason_code) << ","
             << "\"reason\":" << jsonStr(toString(ev.reason)) << ","
             << "\"source\":" << jsonStr(toString(ev.source)) << ","
             << "\"source_detail\":" << jsonStr(ev.source_detail) << ","
             << "\"details_json\":" << jsonStr(ev.details_json) << "}";
    }
    body << "],";

    body << "\"incidents\":[";
    for (size_t i = 0; i < payload.incidents.size(); ++i) {
        const auto& inc = payload.incidents[i];
        if (i) body << ",";
        const std::vector<std::string> evidence =
            (i < payload.incident_evidence.size()) ? payload.incident_evidence[i]
                                                   : std::vector<std::string>{};
        body << "{"
             << "\"incident_id\":" << jsonStr(inc.incident_id) << ","
             << "\"site_id\":" << jsonStr(inc.site_id) << ","
             << "\"gateway_id\":" << jsonStr(inc.gateway_id) << ","
             << "\"started_at_ms\":" << inc.started_at_ms << ","
             << "\"last_event_ms\":" << inc.last_event_ms << ","
             << "\"resolved_at_ms\":"
             << (inc.resolved_at_ms.has_value() ? std::to_string(*inc.resolved_at_ms)
                                                : std::string("null"))
             << ","
             << "\"affected_devices\":" << inc.affected_devices << ","
             << "\"state\":" << jsonStr(toString(inc.state)) << ","
             << "\"suspected_cause\":"
             << (inc.suspected_cause.has_value() ? jsonStr(*inc.suspected_cause)
                                                 : std::string("null"))
             << ","
             << "\"evidence_event_ids\":" << jsonStringArray(evidence) << "}";
    }
    body << "],";

    body << "\"baselines\":[";
    for (size_t i = 0; i < payload.baselines.size(); ++i) {
        const auto& b = payload.baselines[i];
        if (i) body << ",";
        body << "{"
             << "\"site_id\":" << jsonStr(b.key.site_id) << ","
             << "\"gateway_id\":" << jsonStr(b.key.gateway_id) << ","
             << "\"hci_index\":" << b.key.hci_index << ","
             << "\"protocol\":" << jsonStr(toString(b.key.protocol)) << ","
             << "\"address_type\":" << jsonStr(toString(b.key.address_type)) << ","
             << "\"device_address\":" << jsonStr(b.key.device_address) << ","
             << "\"baseline_rssi_dbm\":" << jsonNullableInt(b.baseline_rssi_dbm) << ","
             << "\"min_seen_rssi_dbm\":" << jsonNullableInt(b.min_seen_rssi_dbm) << ","
             << "\"max_seen_rssi_dbm\":" << jsonNullableInt(b.max_seen_rssi_dbm) << ","
             << "\"baseline_sample_count\":" << b.baseline_sample_count << ","
             << "\"state\":" << jsonStr(toString(b.state)) << "}";
    }
    body << "],";

    // 环境窗口：承载深度内核快照（进程画像 Top N + 协议栈丢包归因）。
    // 只有确实取到数据时才 available=true——绝不用空数组冒充"观测过且正常"。
    const bool has_process_data = !payload.top_processes.empty();
    const bool has_drop_data = payload.drop_stats.has_value();
    body << "\"env_window\":{"
         << "\"available\":" << ((has_process_data || has_drop_data) ? "true" : "false") << ","
         << "\"link_type\":\"UNKNOWN\","
         << "\"wifi_anomaly\":false,"
         << "\"coexistence_warning\":false,"
         << "\"from_ms\":" << payload.env_from_ms << ","
         << "\"to_ms\":" << payload.env_to_ms << ","
         << "\"snapshots\":[";

    bool first_snapshot = true;
    if (has_process_data) {
        body << "{\"kind\":\"process_top\",\"ts_ms\":" << payload.env_to_ms << ",\"top_processes\":[";
        for (size_t i = 0; i < payload.top_processes.size(); ++i) {
            const auto& p = payload.top_processes[i];
            if (i) body << ",";
            body << "{\"pid\":" << p.pid
                 << ",\"comm\":" << jsonStr(p.comm)
                 << ",\"tx_bytes\":" << p.txBytes
                 << ",\"tx_packets\":" << p.txPackets
                 << ",\"retrans_count\":" << p.retransCount << "}";
        }
        body << "]}";
        first_snapshot = false;
    }
    if (has_drop_data) {
        const auto& ds = *payload.drop_stats;
        if (!first_snapshot) body << ",";
        body << "{\"kind\":\"skb_drop_hist\",\"ts_ms\":" << payload.env_to_ms
             << ",\"total_drops\":" << ds.totalDrops << ",\"top_reasons\":[";
        for (size_t i = 0; i < ds.topReasons.size(); ++i) {
            const auto& r = ds.topReasons[i];
            if (i) body << ",";
            body << "{\"reason_code\":" << r.reasonCode
                 << ",\"reason_name\":" << jsonStr(r.reasonName)
                 << ",\"description\":" << jsonStr(r.humanDesc)
                 << ",\"protocol\":" << jsonStr(r.protocol)
                 << ",\"count\":" << r.count
                 << ",\"last_timestamp_ns\":" << r.lastTimestampNs << "}";
        }
        body << "]}";
    }

    body << "]}}";
    return body.str();
}

WirelessUplinkPayload EdgeWirelessUplinkExporter::selectSince(const WirelessUplinkPayload& all,
                                                              uint64_t watermark_ms) {
    WirelessUplinkPayload out = all;
    out.events.clear();
    for (const auto& ev : all.events) {
        if (ev.timestamp_ms > watermark_ms) out.events.push_back(ev);
    }
    return out;
}

// ============================================================================
// 构造 / 生命周期
// ============================================================================

EdgeWirelessUplinkExporter::EdgeWirelessUplinkExporter(
    const weaknet_dbus::WeakNetConfig& config,
    weaknet_dbus::DatabaseManager& db,
    std::string state_path,
    std::function<weaknet_dbus::ProcessNetProfiler*()> profiler_provider,
    std::function<weaknet_dbus::SkbDropMonitor*()> drop_provider)
    : config_(config),
      db_(db),
      state_path_(std::move(state_path)),
      profiler_provider_(std::move(profiler_provider)),
      drop_provider_(std::move(drop_provider)) {}

EdgeWirelessUplinkExporter::~EdgeWirelessUplinkExporter() { stop(); }

bool EdgeWirelessUplinkExporter::configComplete(std::string* error) const {
    if (!config_.edge.enabled.load()) {
        *error = "edge.enabled=false";
        return false;
    }
    if (config_.edge.url.get().empty()) {
        *error = "edge.url 未配置";
        return false;
    }
    if (config_.edge.device_id.get().empty()) {
        *error = "edge.device_id 未配置";
        return false;
    }
    if (config_.edge.tenant.get().empty() || config_.edge.token.get().empty() ||
        config_.edge.key_id.get().empty() || config_.edge.private_key_path.get().empty()) {
        *error = "edge 凭证字段不完整";
        return false;
    }
    return true;
}

bool EdgeWirelessUplinkExporter::start() {
    if (running_.load()) return true;

    std::string error;
    if (!configComplete(&error)) {
        LOG_WARNING(weaknet_dbus::LogModule::SYSTEM,
                    "无线事实上行未启用: " << error);
        return false;
    }
#ifndef WEAKNET_HAVE_TLS
    LOG_ERROR(weaknet_dbus::LogModule::SYSTEM,
              "无线事实上行需要 Ed25519 签名，但本构建未编译 OpenSSL，已保持关闭");
    return false;
#else
#ifndef WEAKNET_HAVE_CURL
    LOG_ERROR(weaknet_dbus::LogModule::SYSTEM,
              "无线事实上行需要 libcurl，但本构建未链接，已保持关闭");
    return false;
#else
    watermark_ms_.store(loadWatermark());
    stop_requested_.store(false);
    running_.store(true);
    thread_ = std::thread(&EdgeWirelessUplinkExporter::run, this);
    LOG_INFO(weaknet_dbus::LogModule::SYSTEM,
             "无线事实上行已启动: watermark_ms=" << watermark_ms_.load()
             << " state=" << state_path_);
    return true;
#endif
#endif
}

void EdgeWirelessUplinkExporter::stop() {
    if (!running_.load() && !thread_.joinable()) return;
    stop_requested_.store(true);
    if (thread_.joinable()) thread_.join();
    running_.store(false);
    LOG_INFO(weaknet_dbus::LogModule::SYSTEM, "无线事实上行已停止");
}

EdgeWirelessUplinkStats EdgeWirelessUplinkExporter::stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

uint64_t EdgeWirelessUplinkExporter::watermarkMs() const { return watermark_ms_.load(); }

// ============================================================================
// 游标
// ============================================================================

uint64_t EdgeWirelessUplinkExporter::loadWatermark() const {
    std::ifstream in(state_path_);
    if (!in) return 0;
    std::ostringstream oss;
    oss << in.rdbuf();
    const std::string text = oss.str();

    const std::string ev = readUintField(text, "events_ms");
    if (ev.empty()) return 0;
    try {
        return static_cast<uint64_t>(std::stoull(ev));
    } catch (...) {
        // 损坏即从 0 重扫：重发由服务端幂等吸收，跳发才会真的丢事实。
        LOG_WARNING(weaknet_dbus::LogModule::SYSTEM,
                    "无线上行游标损坏，从 0 重新扫描: " << state_path_);
        return 0;
    }
}

void EdgeWirelessUplinkExporter::storeWatermark(uint64_t value) {
    std::ofstream out(state_path_, std::ios::trunc);
    if (!out) {
        LOG_WARNING(weaknet_dbus::LogModule::SYSTEM,
                    "无线上行游标写入失败（下一轮将重发）: " << state_path_);
        return;
    }
    out << "events_ms=" << value << "\n";
}

// ============================================================================
// 采集
// ============================================================================

bool EdgeWirelessUplinkExporter::collect(WirelessUplinkPayload* out, std::string* error) {
    const uint64_t watermark = watermark_ms_.load();
    out->device_id = config_.edge.device_id.get();
    out->watermark_ms = watermark;

    const int64_t since = static_cast<int64_t>(watermark);
    const int64_t now_ms = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());

    // 事件：只取晚于游标的（严格大于，避免重发已确认项）
    const auto events = db_.loadDeviceEventsForReplay(since + 1, now_ms);
    if (events.size() > EDGE_WIRELESS_MAX_EVENTS_PER_BATCH) {
        out->events.assign(events.begin(),
                           events.begin() + EDGE_WIRELESS_MAX_EVENTS_PER_BATCH);
    } else {
        out->events = events;
    }

    // 事故：按最近演化时间取（LAST_EVENT 会前进，同一事故被反复 UPSERT）
    auto incidents = db_.loadSiteIncidentsSince(since, EDGE_WIRELESS_MAX_INCIDENTS_PER_BATCH);
    out->incident_evidence.reserve(incidents.size());
    for (const auto& inc : incidents) {
        out->incident_evidence.push_back(db_.loadIncidentEvidenceEventIds(inc.incident_id));
    }
    out->incidents = std::move(incidents);

    // 基线：按画像更新时间推进（与事件游标共用同一水位的近似上界即可：
    // 基线是派生事实，重发代价低，宁可多发不可漏发）
    auto baselines = db_.loadDeviceBaselinesSince(since, EDGE_WIRELESS_MAX_BASELINES_PER_BATCH);
    out->baselines = std::move(baselines);

    // 深度内核快照：进程画像 Top N 与协议栈丢包归因。
    // provider 为空 / 返回 nullptr（未启用、加载失败或运行期被 disable）时
    // 诚实跳过该维度，绝不伪造空观测。
    if (profiler_provider_) {
        if (auto* profiler = profiler_provider_(); profiler && profiler->isAvailable()) {
            out->top_processes = profiler->getTopBandwidth(5);
        }
    }
    if (drop_provider_) {
        if (auto* drop_mon = drop_provider_(); drop_mon && drop_mon->isAvailable()) {
            auto drop_stats = drop_mon->getDropStats();
            // 只为有效数据上报：全 0 且无原因项视为"无观测"，不发送空壳
            if (drop_stats.totalDrops > 0 || !drop_stats.topReasons.empty()) {
                if (drop_stats.topReasons.size() > 5) {
                    drop_stats.topReasons.resize(5);
                }
                out->drop_stats = std::move(drop_stats);
            }
        }
    }

    // 环境窗口的时间轴：**只由稀疏事实（事件/事故）决定**，不用墙钟，
    // 也不用基线画像时间。
    //
    // 用墙钟会让每一轮产生新的 to_ms → 云端 window_id 每轮都变 → 每 10 秒新建
    // 一行（实测 10 分钟涨到 606 行）。而基线画像 `updated_at_ms` 每轮都会刷新
    // （设备一直被重新观测），把它算进来同样会让窗口每轮漂移——实测只降到
    // 3 行/分钟。
    //
    // 事件与事故才是真正的稀疏事实：没有它们时窗口恒为 (0, 0)，云端 UPSERT
    // 到同一行；有新事件时才产生新窗口。这既让行数与真实事实量成正比，
    // 也让回放一批旧事件时窗口回到对应时间段，不会把历史事实伪造成"当前窗口"。
    uint64_t newest_fact_ms = 0;
    for (const auto& ev : out->events) newest_fact_ms = std::max(newest_fact_ms, ev.timestamp_ms);
    for (const auto& inc : out->incidents) newest_fact_ms = std::max(newest_fact_ms, inc.last_event_ms);
    out->env_from_ms = 0;
    out->env_to_ms = newest_fact_ms;

    if (out->events.empty() && out->incidents.empty() && out->baselines.empty()) {
        if (error) *error = "no new facts";
        return false;
    }
    return true;
}

// ============================================================================
// 签名 / 发送
// ============================================================================

std::string EdgeWirelessUplinkExporter::signBody(const std::string& body,
                                                 std::string* error) const {
#ifdef WEAKNET_HAVE_TLS
    FILE* fp = std::fopen(config_.edge.private_key_path.get().c_str(), "rb");
    if (!fp) {
        *error = "无法打开私钥: " + config_.edge.private_key_path.get();
        return {};
    }
    EVP_PKEY* key = PEM_read_PrivateKey(fp, nullptr, nullptr, nullptr);
    std::fclose(fp);
    if (!key) {
        *error = "PEM 解析失败";
        return {};
    }
    if (EVP_PKEY_base_id(key) != EVP_PKEY_ED25519) {
        EVP_PKEY_free(key);
        *error = "私钥不是 Ed25519";
        return {};
    }

    std::string signature_hex;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    do {
        if (!ctx) {
            *error = "EVP_MD_CTX_new 失败";
            break;
        }
        if (EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, key) != 1) {
            *error = "EVP_DigestSignInit 失败";
            break;
        }
        size_t sig_len = 0;
        if (EVP_DigestSign(ctx, nullptr, &sig_len,
                           reinterpret_cast<const unsigned char*>(body.data()),
                           body.size()) != 1) {
            *error = "签名长度探测失败";
            break;
        }
        std::vector<unsigned char> sig(sig_len);
        if (EVP_DigestSign(ctx, sig.data(), &sig_len,
                           reinterpret_cast<const unsigned char*>(body.data()),
                           body.size()) != 1) {
            *error = "签名失败";
            break;
        }
        static const char* kHex = "0123456789abcdef";
        signature_hex.reserve(sig_len * 2);
        for (size_t i = 0; i < sig_len; ++i) {
            signature_hex.push_back(kHex[(sig[i] >> 4) & 0x0F]);
            signature_hex.push_back(kHex[sig[i] & 0x0F]);
        }
    } while (false);

    if (ctx) EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(key);
    return signature_hex;
#else
    (void)body;
    *error = "本构建未编译 OpenSSL，无法签名";
    return {};
#endif
}

std::string EdgeWirelessUplinkExporter::uplinkUrl() const {
    std::string url = config_.edge.url.get();
    const std::string suffix = "/telemetry";
    if (url.size() >= suffix.size() &&
        url.compare(url.size() - suffix.size(), suffix.size(), suffix) == 0) {
        url.replace(url.size() - suffix.size(), suffix.size(), "/wireless-events");
    } else {
        // 形态不符约定时显式失败，不把事实发到未知端点
        url.clear();
    }
    return url;
}

bool EdgeWirelessUplinkExporter::postSigned(const std::string& url, const std::string& body,
                                            const std::string& signature,
                                            std::string* response,
                                            std::string* error) const {
#ifdef WEAKNET_HAVE_CURL
    CURL* curl = curl_easy_init();
    if (!curl) {
        *error = "curl_easy_init 失败";
        return false;
    }
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    const std::string key_header = "X-Edge-Key-Id: " + config_.edge.key_id.get();
    const std::string token_header = "X-Edge-Token: " + config_.edge.token.get();
    const std::string tenant_header = "X-Edge-Tenant: " + config_.edge.tenant.get();
    const std::string sig_header = "X-Edge-Signature: " + signature;
    headers = curl_slist_append(headers, key_header.c_str());
    headers = curl_slist_append(headers, token_header.c_str());
    headers = curl_slist_append(headers, tenant_header.c_str());
    headers = curl_slist_append(headers, sig_header.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,
                     static_cast<long>(config_.edge.timeout_ms.load()));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS,
                     static_cast<long>(config_.edge.timeout_ms.load()));

    const CURLcode code = curl_easy_perform(curl);
    long http_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (code != CURLE_OK || http_status < 200 || http_status >= 300) {
        *error = std::string("上行失败: ") + curl_easy_strerror(code) +
                 " http=" + std::to_string(http_status);
        return false;
    }
    return true;
#else
    (void)url; (void)body; (void)signature; (void)response;
    *error = "本构建未链接 libcurl，无法发送";
    return false;
#endif
}

// ============================================================================
// 主循环
// ============================================================================

bool EdgeWirelessUplinkExporter::runOnce(std::string* error) {
    WirelessUplinkPayload payload;
    if (!collect(&payload, error)) {
        return false;
    }

    std::string body_error;
    const std::string body = buildBody(payload, &body_error);
    if (body.empty()) {
        *error = "序列化失败: " + body_error;
        return false;
    }

    std::string sign_error;
    const std::string signature = signBody(body, &sign_error);
    if (signature.empty()) {
        *error = "签名失败: " + sign_error;
        return false;
    }

    const std::string url = uplinkUrl();
    if (url.empty()) {
        *error = "wireless-events URL 无法由 edge.url 派生";
        return false;
    }

    std::string response;
    if (!postSigned(url, body, signature, &response, error)) {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.send_failures++;
        return false;
    }

    // 只有确认送达才推进游标：取本批各行事实时间的最大值。
    uint64_t advanced = watermark_ms_.load();
    for (const auto& ev : payload.events) {
        advanced = std::max(advanced, ev.timestamp_ms);
    }
    for (const auto& inc : payload.incidents) {
        advanced = std::max(advanced, inc.last_event_ms);
    }
    for (const auto& b : payload.baselines) {
        advanced = std::max(advanced, b.updated_at_ms);
    }
    watermark_ms_.store(advanced);
    storeWatermark(advanced);

    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.rounds++;
        stats_.batches_sent++;
        stats_.events_sent += payload.events.size();
        stats_.incidents_sent += payload.incidents.size();
        stats_.baselines_sent += payload.baselines.size();
    }
    LOG_INFO(weaknet_dbus::LogModule::SYSTEM,
             "无线事实上行成功: events=" << payload.events.size()
             << " incidents=" << payload.incidents.size()
             << " baselines=" << payload.baselines.size()
             << " watermark_ms=" << advanced);
    return true;
}

void EdgeWirelessUplinkExporter::run() {
    while (!stop_requested_.load()) {
        // 与遥测同款退避：网络长期不通时不要变成紧循环。
        uint32_t interval = config_.edge.interval_ms.load();
        if (interval < kUplinkIntervalMsMin) interval = kUplinkIntervalMsMin;
        if (interval > kUplinkIntervalMsMax) interval = kUplinkIntervalMsMax;

        std::string error;
        const bool ok = runOnce(&error);
        if (!ok) {
            // "no new facts" 是正常空转，不记错误日志
            if (error != "no new facts") {
                LOG_WARNING(weaknet_dbus::LogModule::SYSTEM, "无线事实上行未送达: " << error);
                interval = std::min(std::max(interval, 1000u), 10000u);
            } else {
                interval = std::max(interval, 1000u);
            }
        } else {
            // 成功但可能仍有积压（超过单批上限）：立刻再跑一轮
            WirelessUplinkPayload probe;
            std::string probe_error;
            if (collect(&probe, &probe_error) && !probe.events.empty()) {
                continue;
            }
        }

        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(interval);
        while (!stop_requested_.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

}  // namespace weaknet
