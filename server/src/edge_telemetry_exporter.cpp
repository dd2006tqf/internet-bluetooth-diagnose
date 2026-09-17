/**
 * @file edge_telemetry_exporter.cpp
 * @brief 边缘遥测上报器实现（libcurl + OpenSSL Ed25519 + 固定容量环形缓冲）
 *
 * 设计约束与理由见头文件。实现要点：
 *
 *   1. **签名覆盖发送字节**：buildRecord() 先序列化 body，再对这段
 *      std::string 直接签名；transmit() 原样发送。中间不做任何重新序列化。
 *
 *   2. **失败即保留**：HTTP 失败不丢弃记录，留在缓冲里等下一轮。弱网下
 *      "上报失败"是常态，丢弃等于在故障最需要证据时丢掉证据。
 *
 *   3. **响应丢失也安全**：服务端按 (device, epoch, sequence) 幂等，
 *      因此重发重复数据是安全的，不需要本端做去重确认。
 *
 *   4. **下行动作只走白名单**：服务端下发的配置变更交给既有
 *      setMonitorParam()，复用其类型/范围校验；不 shell out，不绕过校验。
 */

#include "edge_telemetry_exporter.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <utility>

#ifdef WEAKNET_HAVE_CURL
#include <curl/curl.h>
#endif

#include "assurance/edge_telemetry_serializer.hpp"
#include "logger.hpp"
#include "utils/json_escape.hpp"
#include "weaknet_config.hpp"

#ifdef WEAKNET_HAVE_TLS
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
#include <openssl/provider.h>
#endif
#endif

namespace weaknet {

namespace {

#ifdef WEAKNET_HAVE_CURL
/// libcurl 写回调：把响应体追加到 std::string。
size_t writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    const size_t total = size * nmemb;
    static_cast<std::string*>(userdata)->append(ptr, total);
    return total;
}

/// libcurl 全局初始化（进程内仅需一次，且需线程安全）。
void ensureCurlGlobalInit() {
    static std::once_flag once;
    std::call_once(once, []() {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    });
}
#endif  // WEAKNET_HAVE_CURL

/**
 * @brief 从 PEM 文件读取 Ed25519 私钥。
 *
 * 支持 PEM 中的 PKCS#8（BEGIN PRIVATE KEY）与 OpenSSL 传统格式
 * （BEGIN PRIVATE KEY 之外的封装由 OpenSSL 自行识别）。密钥类型不是
 * Ed25519 时直接失败——绝不用错算法继续跑。
 */
#ifdef WEAKNET_HAVE_TLS
EVP_PKEY* loadEd25519PrivateKey(const std::string& path, std::string* error) {
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) {
        *error = "无法打开私钥文件: " + path + " (" + std::strerror(errno) + ")";
        return nullptr;
    }
    EVP_PKEY* key = PEM_read_PrivateKey(fp, nullptr, nullptr, nullptr);
    std::fclose(fp);
    if (!key) {
        *error = "PEM 解析失败: " + path;
        return nullptr;
    }
    if (EVP_PKEY_base_id(key) != EVP_PKEY_ED25519) {
        EVP_PKEY_free(key);
        *error = "私钥不是 Ed25519 类型: " + path;
        return nullptr;
    }
    return key;
}

/// 把裸签名转成小写 hex（与 Python 端 bytes.hex() 一致）。
std::string toHex(const unsigned char* data, size_t len) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(kHex[(data[i] >> 4) & 0x0F]);
        out.push_back(kHex[data[i] & 0x0F]);
    }
    return out;
}
#endif  // WEAKNET_HAVE_TLS

/// 把毫秒时间戳格式化为 RFC3339 UTC（服务端 Pydantic datetime 需要时区）。
std::string formatRfc3339Utc(int64_t epoch_ms) {
    const std::time_t seconds = static_cast<std::time_t>(epoch_ms / 1000);
    // 负余数会得到负的毫秒位（1970 之前），归一到 [0,1000) 保证格式合法。
    int millis = static_cast<int>(epoch_ms % 1000);
    if (millis < 0) millis += 1000;
    std::tm tm_utc{};
    if (gmtime_r(&seconds, &tm_utc) == nullptr) {
        return "1970-01-01T00:00:00.000Z";
    }
    // 最长形态 "YYYY-MM-DDTHH:MM:SS.mmmZ" = 24 字节；取 32 留足余量，
    // 且截断在格式上不可能发生（年份被 %04d 限制在 4 位）。
    char buf[32];
    const int written = std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                                      tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
                                      tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec, millis);
    if (written < 0 || static_cast<size_t>(written) >= sizeof(buf)) {
        return "1970-01-01T00:00:00.000Z";
    }
    return std::string(buf, static_cast<size_t>(written));
}

/// 极简 JSON 字段提取：取 `"key":` 之后的字符串字面量值。
///
/// 只用于读取**本端自己服务端**响应里的 pending_actions，不做通用解析。
/// 拿不准的一律返回空/跳过，绝不猜测。
bool extractStringField(const std::string& json, const std::string& key, size_t from,
                        std::string* out, size_t* next) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle, from);
    if (pos == std::string::npos) return false;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return false;
    pos = json.find('"', pos);
    if (pos == std::string::npos) return false;
    ++pos;
    std::string value;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) ++pos;
        value.push_back(json[pos++]);
    }
    if (pos >= json.size()) return false;
    *out = value;
    if (next) *next = pos;
    return true;
}

/// 从响应体中提取全部 pending_actions（动作对象数组，保序）。
///
/// 每个动作对象携带 {action_id, key, value} 三个字段；action_id 是回传
/// 结果时的唯一凭据，必须与 key/value 一并取出，否则无法回执。
struct PendingAction {
    std::string action_id;
    std::string key;
    std::string value;
};

std::vector<PendingAction> parsePendingActions(const std::string& body) {
    std::vector<PendingAction> actions;
    const std::string array_key = "\"pending_actions\"";
    size_t pos = body.find(array_key);
    if (pos == std::string::npos) return actions;

    while (true) {
        PendingAction action;
        size_t cursor = pos;
        // 每个动作对象内同时含 action_id / key / value；用 action_id 作为
        // 分段锚点，避免跨对象误取。
        if (!extractStringField(body, "action_id", cursor, &action.action_id, &cursor)) break;
        // 该动作对象在本段内查找 key/value；边界取下一个 action_id 之前。
        const size_t next_action = body.find("\"action_id\"", cursor);
        const size_t segment_end = (next_action == std::string::npos) ? body.size() : next_action;
        const std::string segment = body.substr(cursor, segment_end - cursor);
        if (!extractStringField(segment, "key", 0, &action.key, nullptr)) { pos = segment_end; continue; }
        if (!extractStringField(segment, "value", 0, &action.value, nullptr)) { pos = segment_end; continue; }
        actions.push_back(std::move(action));
        pos = segment_end;
        if (next_action == std::string::npos) break;
    }
    return actions;
}

}  // namespace

EdgeTelemetryExporter::EdgeTelemetryExporter(const weaknet_dbus::WeakNetConfig& config,
                                            std::string hostname)
    : config_(config), hostname_(std::move(hostname)) {}

EdgeTelemetryExporter::~EdgeTelemetryExporter() {
    stop();
}

bool EdgeTelemetryExporter::configComplete(std::string* error) const {
    if (config_.edge.url.get().empty()) {
        *error = "edge.url 未配置";
        return false;
    }
    if (config_.edge.tenant.get().empty()) {
        *error = "edge.tenant 未配置";
        return false;
    }
    if (config_.edge.device_id.get().empty()) {
        *error = "edge.device_id 未配置";
        return false;
    }
    if (config_.edge.token.get().empty()) {
        *error = "edge.token 未配置";
        return false;
    }
    if (config_.edge.private_key_path.get().empty()) {
        *error = "edge.private_key_path 未配置";
        return false;
    }
    if (config_.edge.key_id.get().empty()) {
        *error = "edge.key_id 未配置";
        return false;
    }
    return true;
}

bool EdgeTelemetryExporter::start() {
    if (running_.load()) return true;
    if (!config_.edge.enabled.load()) {
        LOG_INFO(weaknet_dbus::LogModule::SYSTEM, "边缘遥测上报未启用（edge.enabled=false）");
        return false;
    }

    std::string error;
    if (!configComplete(&error)) {
        // 半配置状态必须显式失败：否则会以空身份对外发数据。
        LOG_ERROR(weaknet_dbus::LogModule::SYSTEM, "边缘遥测上报配置不完整，已保持关闭: " << error);
        return false;
    }

#ifndef WEAKNET_HAVE_TLS
    LOG_ERROR(weaknet_dbus::LogModule::SYSTEM,
              "边缘遥测上报需要 Ed25519 签名，但本构建未编译 OpenSSL，已保持关闭");
    return false;
#else
    // 启动即验证私钥可用：把"密钥配错"暴露在启动日志里，而不是每轮上报
    // 都失败却只留下噪音。
    {
        std::string key_error;
        EVP_PKEY* probe = loadEd25519PrivateKey(config_.edge.private_key_path.get(), &key_error);
        if (!probe) {
            LOG_ERROR(weaknet_dbus::LogModule::SYSTEM, "边缘遥测上报私钥不可用，已保持关闭: " << key_error);
            return false;
        }
        EVP_PKEY_free(probe);
    }

#ifndef WEAKNET_HAVE_CURL
    // 没有 libcurl 就不能发请求。显式失败，绝不"签名了但发不出去还假装在跑"。
    LOG_ERROR(weaknet_dbus::LogModule::SYSTEM,
              "边缘遥测上报需要 libcurl，但本构建未链接，已保持关闭");
    return false;
#else
    ensureCurlGlobalInit();

    stop_requested_.store(false);
    running_.store(true);
    thread_ = std::thread([this]() { run(); });
    LOG_INFO(weaknet_dbus::LogModule::SYSTEM,
             "边缘遥测上报已启动: device=" << config_.edge.device_id.get()
             << " interval_ms=" << config_.edge.interval_ms.load()
             << " buffer=" << EDGE_TELEMETRY_BUFFER_CAPACITY);
    return true;
#endif  // WEAKNET_HAVE_CURL
#endif  // WEAKNET_HAVE_TLS
}

void EdgeTelemetryExporter::stop() {
    if (!running_.load() && !thread_.joinable()) return;
    stop_requested_.store(true);
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    running_.store(false);
    LOG_INFO(weaknet_dbus::LogModule::SYSTEM, "边缘遥测上报已停止");
}

bool EdgeTelemetryExporter::enqueue(const AssessmentSnapshot& snapshot) {
    if (!running_.load()) return false;

    EdgeTelemetryRecord record;
    std::string error;
    if (!buildRecord(snapshot, &record, &error)) {
        // 序列化/签名失败不应影响评估主循环，只记日志并跳过这一条。
        LOG_ERROR(weaknet_dbus::LogModule::SYSTEM, "边缘遥测记录构造失败: " << error);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (buffer_.size() >= EDGE_TELEMETRY_BUFFER_CAPACITY) {
            // 覆盖最旧：保留最近的状态比保留历史更符合"当前故障是什么"的用途。
            buffer_.pop_front();
            std::lock_guard<std::mutex> slock(stats_mutex_);
            stats_.dropped_oldest++;
        }
        buffer_.push_back(std::move(record));
        std::lock_guard<std::mutex> slock(stats_mutex_);
        stats_.snapshots_enqueued++;
        stats_.buffered = buffer_.size();
    }
    cv_.notify_one();
    return true;
}

EdgeExporterStats EdgeTelemetryExporter::stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    EdgeExporterStats copy = stats_;
    {
        std::lock_guard<std::mutex> block(mutex_);
        copy.buffered = buffer_.size();
    }
    return copy;
}

bool EdgeTelemetryExporter::buildRecord(const AssessmentSnapshot& snapshot,
                                       EdgeTelemetryRecord* out, std::string* error) {
    const auto& exp = snapshot.experience;

    std::ostringstream body;
    body << "{\"schema_version\":\"network.edge.telemetry.v1\",\"snapshots\":[{";
    body << "\"device_id\":\"" << weaknet_utils::escapeJsonString(config_.edge.device_id.get()) << "\",";
    body << "\"display_name\":\"" << weaknet_utils::escapeJsonString(hostname_) << "\",";
    body << "\"sequence_id\":" << snapshot.sequence_id << ",";
    body << "\"network_epoch\":" << snapshot.network_epoch << ",";
    body << "\"config_generation\":" << snapshot.config_generation << ",";

    const auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        snapshot.wall_timestamp.time_since_epoch()).count();
    body << "\"captured_at\":\"" << formatRfc3339Utc(wall_ms) << "\",";

    // 专用序列化器：直接产出上报契约的嵌套形态。
    // 不复用 toExperienceJsonV2 再改写形状——那等于对刚产出的 JSON 做字符串
    // 手术，脆弱且会让两份契约互相污染（见 serializer 头文件说明）。
    body << "\"snapshot\":" << toEdgeTelemetrySnapshotJson(exp) << "}]}";

    out->sequence_id = snapshot.sequence_id;
    out->network_epoch = snapshot.network_epoch;
    out->config_generation = snapshot.config_generation;
    out->wall_timestamp_ms = wall_ms;
    out->body = body.str();
    out->signature = signBody(out->body, error);
    return !out->signature.empty();
}

std::string EdgeTelemetryExporter::signBody(const std::string& body, std::string* error) {
#ifdef WEAKNET_HAVE_TLS
    EVP_PKEY* key = loadEd25519PrivateKey(config_.edge.private_key_path.get(), error);
    if (!key) return {};

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        EVP_PKEY_free(key);
        *error = "EVP_MD_CTX_new 失败";
        return {};
    }

    // Ed25519 是"一次性签名"算法：必须用 EVP_DigestSign 的单次形式，
    // 传 NULL 摘要，不能先 update 再 final。1.1.1 与 3.x 均支持此路径。
    std::string signature_hex;
    do {
        if (EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, key) != 1) {
            *error = "EVP_DigestSignInit 失败";
            break;
        }
        size_t sig_len = 0;
        if (EVP_DigestSign(ctx, nullptr, &sig_len,
                           reinterpret_cast<const unsigned char*>(body.data()),
                           body.size()) != 1) {
            *error = "EVP_DigestSign(长度探测) 失败";
            break;
        }
        std::vector<unsigned char> sig(sig_len);
        if (EVP_DigestSign(ctx, sig.data(), &sig_len,
                           reinterpret_cast<const unsigned char*>(body.data()),
                           body.size()) != 1) {
            *error = "EVP_DigestSign 失败";
            break;
        }
        signature_hex = toHex(sig.data(), sig_len);
    } while (false);

    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(key);
    return signature_hex;
#else
    *error = "本构建未编译 OpenSSL，无法签名";
    return {};
#endif
}

bool EdgeTelemetryExporter::postSigned(const std::string& url, const std::string& body,
                                        const std::string& signature, std::string* response,
                                        std::string* error) {
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
    // 显式超时：绝不允许上报线程因对端无响应而长时间挂起。
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
        *error = std::string("上报失败: ") + curl_easy_strerror(code) +
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

bool EdgeTelemetryExporter::transmit(const std::vector<EdgeTelemetryRecord>& records,
                                    std::string* error) {
    if (records.empty()) return true;

#ifdef WEAKNET_HAVE_CURL
    // 批量补发时，body 取"最后一条"的签名对应的字节；因此批量场景下
    // 每条记录单独发送，而不是拼成一个巨大请求——拼接会破坏
    // "签名覆盖发送字节"这一不变式。
    for (const auto& record : records) {
        std::string response;
        if (!postSigned(config_.edge.url.get(), record.body, record.signature,
                        &response, error)) {
            return false;
        }

        applyPendingActions(response);

        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.snapshots_sent++;
        }
    }
    return true;
#else
    *error = "本构建未链接 libcurl，无法发送";
    return false;
#endif  // WEAKNET_HAVE_CURL
}

void EdgeTelemetryExporter::applyPendingActions(const std::string& response_body) {
    const auto actions = parsePendingActions(response_body);
    if (actions.empty()) return;

    for (const auto& action : actions) {
        std::string error;
        // 复用既有白名单 + 类型 + 范围校验；本层不做任何"宽松处理"。
        const bool ok = weaknet_dbus::setMonitorParam(
            const_cast<weaknet_dbus::WeakNetConfig*>(&config_), action.key, action.value, &error);

        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            if (ok) {
                stats_.actions_applied++;
            } else {
                stats_.actions_rejected++;
            }
        }

        if (ok) {
            LOG_INFO(weaknet_dbus::LogModule::SYSTEM,
                     "已应用服务端下发的配置: " << action.key << "=" << action.value
                     << " (action=" << action.action_id << ")");
        } else {
            LOG_ERROR(weaknet_dbus::LogModule::SYSTEM,
                      "服务端下发配置被拒绝: " << action.key << "=" << action.value
                      << " (action=" << action.action_id << ", " << error << ")");
        }

        // 无论应用成功与否都必须回执：服务端靠它把 action 推进到
        // APPLIED/REJECTED，缺了回执这条 action 会停在 DELIVERED 永远不完结。
        queueActionResult(action.action_id, ok, ok ? "applied" : error);
    }
}

void EdgeTelemetryExporter::queueActionResult(const std::string& action_id, bool applied,
                                               const std::string& detail) {
    EdgeActionResultRecord rec;
    rec.action_id = action_id;
    rec.status = applied ? "APPLIED" : "REJECTED";
    rec.detail = detail;

    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    std::ostringstream body;
    body << "{\"schema_version\":\"network.edge.action-results.v1\",";
    body << "\"device_id\":\"" << weaknet_utils::escapeJsonString(config_.edge.device_id.get()) << "\",";
    body << "\"results\":[{";
    body << "\"action_id\":\"" << weaknet_utils::escapeJsonString(action_id) << "\",";
    body << "\"status\":\"" << rec.status << "\",";
    body << "\"detail\":\"" << weaknet_utils::escapeJsonString(detail) << "\",";
    body << "\"reported_at\":\"" << formatRfc3339Utc(now_ms) << "\"}]}";
    rec.body = body.str();

    std::string sign_error;
    rec.signature = signBody(rec.body, &sign_error);
    if (rec.signature.empty()) {
        LOG_ERROR(weaknet_dbus::LogModule::SYSTEM,
                  "动作结果签名失败，无法回执: action=" << action_id << " (" << sign_error << ")");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_action_results_.push_back(std::move(rec));
    }
    cv_.notify_one();
}

std::string EdgeTelemetryExporter::actionResultsUrl() const {
    std::string url = config_.edge.url.get();
    const std::string suffix = "/telemetry";
    if (url.size() >= suffix.size() &&
        url.compare(url.size() - suffix.size(), suffix.size(), suffix) == 0) {
        url.replace(url.size() - suffix.size(), suffix.size(), "/action-results");
    } else {
        // URL 形态不符约定时显式失败，而不是把结果发到一个错误的端点。
        url.clear();
    }
    return url;
}

bool EdgeTelemetryExporter::transmitActionResults(std::string* error) {
    std::deque<EdgeActionResultRecord> pending;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending.swap(pending_action_results_);
    }
    if (pending.empty()) return true;

    const std::string url = actionResultsUrl();
    if (url.empty()) {
        *error = "action-results URL 无法由 edge.url 派生";
        // 把结果放回队列，避免丢弃。
        std::lock_guard<std::mutex> lock(mutex_);
        while (!pending.empty()) {
            pending_action_results_.push_front(std::move(pending.back()));
            pending.pop_back();
        }
        return false;
    }

    bool all_ok = true;
    std::deque<EdgeActionResultRecord> unsent;
    for (auto& rec : pending) {
        std::string response;
        std::string send_error;
        if (postSigned(url, rec.body, rec.signature, &response, &send_error)) {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.action_results_sent++;
        } else {
            *error = send_error;
            unsent.push_back(std::move(rec));
            all_ok = false;
        }
    }

    // 未送达的结果放回队列头部，等下一轮重试（服务端对未知 action_id 幂等）。
    if (!unsent.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!unsent.empty()) {
            pending_action_results_.push_front(std::move(unsent.back()));
            unsent.pop_back();
        }
    }
    return all_ok;
}

std::vector<EdgeTelemetryRecord> EdgeTelemetryExporter::drain() {
    std::vector<EdgeTelemetryRecord> out;
    std::lock_guard<std::mutex> lock(mutex_);
    out.reserve(buffer_.size());
    while (!buffer_.empty()) {
        out.push_back(std::move(buffer_.front()));
        buffer_.pop_front();
    }
    std::lock_guard<std::mutex> slock(stats_mutex_);
    stats_.buffered = 0;
    return out;
}

void EdgeTelemetryExporter::run() {
    LOG_INFO(weaknet_dbus::LogModule::SYSTEM, "边缘遥测上报线程已启动");
    while (!stop_requested_.load()) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(config_.edge.interval_ms.load()),
                     [this]() {
                         return !buffer_.empty() || !pending_action_results_.empty() ||
                                stop_requested_.load();
                     });
        lock.unlock();

        if (stop_requested_.load()) break;

        const bool has_action_results = [this]() {
            std::lock_guard<std::mutex> guard(mutex_);
            return !pending_action_results_.empty();
        }();

        auto records = drain();
        if (records.empty() && !has_action_results) continue;

        std::string error;
        const bool telemetry_ok = records.empty() || transmit(records, &error);
        if (!telemetry_ok) {
            std::lock_guard<std::mutex> slock(stats_mutex_);
            stats_.send_failures++;
            // 失败：把未送达的记录放回缓冲**前面**，保持时序。
            // 服务端幂等，因此重发已到达但响应丢失的项也是安全的。
            std::lock_guard<std::mutex> block(mutex_);
            for (auto it = records.rbegin(); it != records.rend(); ++it) {
                if (buffer_.size() >= EDGE_TELEMETRY_BUFFER_CAPACITY) {
                    buffer_.pop_back();
                    std::lock_guard<std::mutex> slock2(stats_mutex_);
                    stats_.dropped_oldest++;
                    break;
                }
                buffer_.push_front(std::move(*it));
            }
            LOG_ERROR(weaknet_dbus::LogModule::SYSTEM, "边缘遥测上报失败（保留待补发）: " << error);
        }

        // 动作结果与遥测共用同一轮唤醒：只要任一通道有积压就尝试回传，
        // 让 APPLIED/REJECTED 尽快到达服务端而不是等下一个遥测周期。
        std::string results_error;
        if (!transmitActionResults(&results_error)) {
            LOG_ERROR(weaknet_dbus::LogModule::SYSTEM,
                      "动作结果回传失败（保留待重发）: " << results_error);
        }
    }
    LOG_INFO(weaknet_dbus::LogModule::SYSTEM, "边缘遥测上报线程已退出");
}

}  // namespace weaknet
