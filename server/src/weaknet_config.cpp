/**
 * @file weaknet_config.cpp
 * @brief YAML 子集配置解析器实现
 *
 * 支持语法（本项目配置文件专用，不是完整 YAML）：
 *   - 注释：以 '#' 开头（整行）；行内 " #..." 也忽略
 *   - 结构：两级缩进 section（server / monitors / <monitor>）
 *   - 值类型：bool / uint / string / 时长（ms/s/m 后缀或裸整数=ms）
 *
 * 失败策略：
 *   - 文件不存在 → 返回 true，out 保持默认值（板上无配置也能跑）
 *   - 语法错误 / 未知 section / 未知字段 → 返回 false，error 带行号
 *     （配置错误必须显眼，不能静默用默认值掩盖）
 */

#include "weaknet_config.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>
#include <set>

#include "utils/json_escape.hpp"

namespace weaknet_dbus {

namespace {

// ---- 小工具 ----

std::string ltrim(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
}

std::string rtrim(const std::string& s) {
    size_t i = s.size();
    while (i > 0 && std::isspace(static_cast<unsigned char>(s[i - 1]))) --i;
    return s.substr(0, i);
}

std::string trim(const std::string& s) { return rtrim(ltrim(s)); }

std::string toLower(const std::string& s) {
    std::string out = s;
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

/// 整行注释或空行
bool isBlankOrComment(const std::string& line) {
    std::string t = ltrim(line);
    return t.empty() || t[0] == '#';
}

/// 截掉行内注释（" value # comment" → " value "），仅当 '#' 前有空白
std::string stripInlineComment(const std::string& s) {
    size_t pos = s.find(" #");
    if (pos != std::string::npos) return s.substr(0, pos);
    return s;
}

bool parseBool(const std::string& v, bool* out) {
    std::string l = toLower(trim(v));
    if (l == "true" || l == "yes" || l == "1") { *out = true; return true; }
    if (l == "false" || l == "no" || l == "0") { *out = false; return true; }
    return false;
}

bool parseUint(const std::string& v, uint32_t* out) {
    if (v.empty()) return false;
    char* end = nullptr;
    unsigned long ul = std::strtoul(v.c_str(), &end, 10);
    if (end == v.c_str() || *end != '\0') return false;
    if (ul > 0xFFFFFFFFUL) return false;
    *out = static_cast<uint32_t>(ul);
    return true;
}

/// 时长解析：支持裸整数（=ms）及 ms/s/m 后缀
bool parseDurationMs(const std::string& v, uint32_t* out) {
    std::string s = trim(v);
    if (s.empty()) return false;
    size_t i = 0;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
    if (i == 0) return false;
    uint32_t num = 0;
    if (!parseUint(s.substr(0, i), &num)) return false;
    std::string unit = s.substr(i);
    uint64_t ms;
    if (unit.empty() || unit == "ms") ms = num;
    else if (unit == "s") ms = static_cast<uint64_t>(num) * 1000;
    else if (unit == "m") ms = static_cast<uint64_t>(num) * 60000;
    else return false;
    if (ms > 0xFFFFFFFFUL) return false;
    *out = static_cast<uint32_t>(ms);
    return true;
}

// ---- 原子字段写入辅助 ----

bool setDurationField(std::atomic<uint32_t>& target, const std::string& val, std::string* error) {
    uint32_t ms;
    if (!parseDurationMs(val, &ms)) {
        *error = "invalid duration: '" + trim(val) + "'";
        return false;
    }
    target.store(ms);
    return true;
}

bool setBoolField(std::atomic<bool>& target, const std::string& val, std::string* error) {
    bool b;
    if (!parseBool(val, &b)) {
        *error = "invalid bool: '" + trim(val) + "'";
        return false;
    }
    target.store(b);
    return true;
}

// ---- 监控器字段分发 ----

/**
 * @brief 将 "monitor.field = value" 写入配置
 * @return false 时 error 已填充（未知监控器 / 未知字段 / 值非法）
 */
bool applyMonitorField(WeakNetConfig* cfg, const std::string& mon,
                       const std::string& field, const std::string& val,
                       std::string* error) {
    if (mon == "rtt") {
        if (field == "enabled") return setBoolField(cfg->rtt.enabled, val, error);
        if (field == "target") { cfg->rtt.target.set(trim(val)); return true; }
        if (field == "interval") return setDurationField(cfg->rtt.interval_ms, val, error);
        if (field == "timeout") return setDurationField(cfg->rtt.timeout_ms, val, error);
        *error = "rtt: unknown field '" + field + "'";
        return false;
    }
    if (mon == "jitter") {
        if (field == "enabled") return setBoolField(cfg->jitter.enabled, val, error);
        if (field == "target") { cfg->jitter.target.set(trim(val)); return true; }
        if (field == "interval") return setDurationField(cfg->jitter.interval_ms, val, error);
        if (field == "timeout") return setDurationField(cfg->jitter.timeout_ms, val, error);
        if (field == "window") return setDurationField(cfg->jitter.window_size, val, error);
        *error = "jitter: unknown field '" + field + "'";
        return false;
    }
    if (mon == "rssi") {
        if (field == "enabled") return setBoolField(cfg->rssi.enabled, val, error);
        if (field == "interval") return setDurationField(cfg->rssi.interval_ms, val, error);
        *error = "rssi: unknown field '" + field + "'";
        return false;
    }
    if (mon == "tcp_loss") {
        if (field == "enabled") return setBoolField(cfg->tcp_loss.enabled, val, error);
        if (field == "interval") return setDurationField(cfg->tcp_loss.interval_ms, val, error);
        *error = "tcp_loss: unknown field '" + field + "'";
        return false;
    }
    if (mon == "traffic") {
        if (field == "enabled") return setBoolField(cfg->traffic.enabled, val, error);
        if (field == "interval") return setDurationField(cfg->traffic.interval_ms, val, error);
        *error = "traffic: unknown field '" + field + "'";
        return false;
    }
    if (mon == "quality") {
        if (field == "enabled") return setBoolField(cfg->quality.enabled, val, error);
        if (field == "interval") return setDurationField(cfg->quality.interval_ms, val, error);
        *error = "quality: unknown field '" + field + "'";
        return false;
    }
    if (mon == "bluetooth") {
        if (field == "enabled") return setBoolField(cfg->bluetooth.enabled, val, error);
        if (field == "interval") return setDurationField(cfg->bluetooth.interval_ms, val, error);
        if (field == "bpf_obj") { cfg->bluetooth.bpf_obj.set(trim(val)); return true; }
        *error = "bluetooth: unknown field '" + field + "'";
        return false;
    }
    if (mon == "dns") {
        if (field == "enabled") return setBoolField(cfg->dns.enabled, val, error);
        if (field == "bpf_obj") { cfg->dns.bpf_obj.set(trim(val)); return true; }
        if (field == "interval") return setDurationField(cfg->dns.interval_ms, val, error);
        *error = "dns: unknown field '" + field + "'";
        return false;
    }
    if (mon == "wifi_loss") {
        if (field == "enabled") return setBoolField(cfg->wifi_loss.enabled, val, error);
        if (field == "bpf_obj") { cfg->wifi_loss.bpf_obj.set(trim(val)); return true; }
        if (field == "interval") return setDurationField(cfg->wifi_loss.interval_ms, val, error);
        *error = "wifi_loss: unknown field '" + field + "'";
        return false;
    }
    if (mon == "http_latency") {
        if (field == "enabled") return setBoolField(cfg->http_latency.enabled, val, error);
        if (field == "bpf_obj") { cfg->http_latency.bpf_obj.set(trim(val)); return true; }
        if (field == "interval") return setDurationField(cfg->http_latency.interval_ms, val, error);
        *error = "http_latency: unknown field '" + field + "'";
        return false;
    }
    if (mon == "process_profiler") {
        if (field == "enabled") return setBoolField(cfg->process_profiler.enabled, val, error);
        if (field == "bpf_obj") { cfg->process_profiler.bpf_obj.set(trim(val)); return true; }
        if (field == "interval") return setDurationField(cfg->process_profiler.interval_ms, val, error);
        *error = "process_profiler: unknown field '" + field + "'";
        return false;
    }
    if (mon == "tcp_retrans") {
        if (field == "enabled") return setBoolField(cfg->tcp_retrans.enabled, val, error);
        if (field == "bpf_obj") { cfg->tcp_retrans.bpf_obj.set(trim(val)); return true; }
        if (field == "interval") return setDurationField(cfg->tcp_retrans.interval_ms, val, error);
        *error = "tcp_retrans: unknown field '" + field + "'";
        return false;
    }
    if (mon == "tcp_conn") {
        if (field == "enabled") return setBoolField(cfg->tcp_conn.enabled, val, error);
        if (field == "bpf_obj") { cfg->tcp_conn.bpf_obj.set(trim(val)); return true; }
        if (field == "interval") return setDurationField(cfg->tcp_conn.interval_ms, val, error);
        *error = "tcp_conn: unknown field '" + field + "'";
        return false;
    }
    *error = "unknown monitor: '" + mon + "'";
    return false;
}

// ---- 服务端 section 字段分发 ----

bool applyServerField(WeakNetConfig* cfg, const std::string& field,
                      const std::string& val, std::string* error) {
    if (field == "dbus_name") { cfg->dbus_name.set(trim(val)); return true; }
    if (field == "data_dir") { cfg->data_dir.set(trim(val)); return true; }
    if (field == "log_level") { cfg->log_level.set(trim(val)); return true; }
    *error = "server: unknown field '" + field + "'";
    return false;
}

/// 解析出的 section 头（保留缩进层级用于闭合）
struct Section {
    int indent;
    std::string name;
};

}  // namespace

bool isEnabledKey(const std::string& key) {
    const std::string suffix = ".enabled";
    if (key.size() <= suffix.size()) return false;
    return key.compare(key.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool splitMonitorKey(const std::string& dotted, std::string* monitor, std::string* field) {
    size_t dot = dotted.find('.');
    if (dot == std::string::npos || dot == 0 || dot == dotted.size() - 1) return false;
    *monitor = dotted.substr(0, dot);
    *field = dotted.substr(dot + 1);
    return true;
}

bool loadWeakNetConfig(const std::string& path, WeakNetConfig* out, std::string* error) {
    if (!out) return false;

    std::ifstream in(path);
    if (!in) {
        // 文件不存在：返回 true，保持默认值（与现有无配置行为一致）
        return true;
    }

    std::vector<Section> stack;   // 当前打开的 section 头栈
    std::string line;
    size_t line_no = 0;

    while (std::getline(in, line)) {
        ++line_no;
        if (isBlankOrComment(line)) continue;

        const size_t indent = line.find_first_not_of(' ');
        const std::string trimmed = trim(stripInlineComment(line));
        if (trimmed.empty()) continue;

        // 以 ':' 结尾 → section 头
        if (!trimmed.empty() && trimmed.back() == ':') {
            std::string name = trim(trimmed.substr(0, trimmed.size() - 1));
            if (name.empty()) {
                *error = "line " + std::to_string(line_no) + ": empty section name";
                return false;
            }
            // 弹出同级或更深层 section
            while (!stack.empty() && stack.back().indent >= static_cast<int>(indent))
                stack.pop_back();
            stack.push_back({static_cast<int>(indent), name});
            continue;
        }

        // key: value 行
        size_t colon = trimmed.find(':');
        if (colon == std::string::npos) {
            *error = "line " + std::to_string(line_no) + ": expected 'key: value', got '" + trimmed + "'";
            return false;
        }
        std::string key = trim(trimmed.substr(0, colon));
        std::string value = trim(trimmed.substr(colon + 1));

        // 弹出同级或更深层 section，当前行属于栈顶
        while (!stack.empty() && stack.back().indent >= static_cast<int>(indent))
            stack.pop_back();

        if (stack.empty()) {
            *error = "line " + std::to_string(line_no) + ": unexpected top-level key '" + key + "'";
            return false;
        }

        const std::string& section = stack.back().name;
        if (section == "server") {
            if (!applyServerField(out, key, value, error)) {
                *error = "line " + std::to_string(line_no) + ": " + *error;
                return false;
            }
        } else if (section == "monitors") {
            *error = "line " + std::to_string(line_no)
                   + ": monitor name must be a section (e.g. 'rtt:'), got inline key '" + key + "'";
            return false;
        } else {
            // monitors 下的监控器名
            if (!applyMonitorField(out, section, key, value, error)) {
                *error = "line " + std::to_string(line_no) + ": " + *error;
                return false;
            }
        }
    }

    return true;
}

// ============================================================================
// 运行时调参与 JSON 序列化（给 D-Bus 方法调用）
// ============================================================================

namespace {

// ---- IPv4 校验（复用 net_ping 的逻辑，这里做轻量版） ----
bool isValidIPv4(const std::string& s) {
    if (s.empty()) return false;
    int dots = 0;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == '.') {
            if (i == start) return false;  // 空段
            if (i - start > 3) return false;
            for (size_t j = start; j < i; ++j) {
                if (!std::isdigit(static_cast<unsigned char>(s[j]))) return false;
            }
            int octet = std::stoi(s.substr(start, i - start));
            if (octet > 255) return false;
            if (i < s.size() && s[i] == '.') ++dots;
            start = i + 1;
        }
    }
    return dots == 3;
}

// ---- 区间校验 ----
bool checkRange(uint32_t v, uint32_t min, uint32_t max) {
    return v >= min && v <= max;
}

}  // anonymous namespace

bool setMonitorParam(WeakNetConfig* cfg, const std::string& key,
                     const std::string& value, std::string* error) {
    if (!cfg) return false;

    std::string mon, field;
    if (!splitMonitorKey(key, &mon, &field)) {
        if (error) *error = "invalid key format (expected 'monitor.field'): " + key;
        return false;
    }

    // 先校验，再原子提交（先在局部变量跑完所有校验，最后一次性写回）
    if (mon == "rtt") {
        if (field == "enabled") { bool b; if (!parseBool(value, &b)) { if (error) *error = "rtt.enabled: invalid bool"; return false; } cfg->rtt.enabled.store(b); return true; }
        if (field == "target") { if (!isValidIPv4(trim(value))) { if (error) *error = "rtt.target: invalid IPv4"; return false; } cfg->rtt.target.set(trim(value)); return true; }
        if (field == "interval") { uint32_t ms; if (!parseDurationMs(value, &ms) || !checkRange(ms, 100, 600000)) { if (error) *error = "rtt.interval: must be 100ms~600000ms"; return false; } cfg->rtt.interval_ms.store(ms); return true; }
        if (field == "timeout") { uint32_t ms; if (!parseDurationMs(value, &ms) || !checkRange(ms, 100, 60000)) { if (error) *error = "rtt.timeout: must be 100ms~60000ms"; return false; } cfg->rtt.timeout_ms.store(ms); return true; }
    }
    if (mon == "jitter") {
        if (field == "enabled") { bool b; if (!parseBool(value, &b)) { if (error) *error = "jitter.enabled: invalid bool"; return false; } cfg->jitter.enabled.store(b); return true; }
        if (field == "target") { if (!isValidIPv4(trim(value))) { if (error) *error = "jitter.target: invalid IPv4"; return false; } cfg->jitter.target.set(trim(value)); return true; }
        if (field == "interval") { uint32_t ms; if (!parseDurationMs(value, &ms) || !checkRange(ms, 100, 600000)) { if (error) *error = "jitter.interval: must be 100ms~600000ms"; return false; } cfg->jitter.interval_ms.store(ms); return true; }
        if (field == "timeout") { uint32_t ms; if (!parseDurationMs(value, &ms) || !checkRange(ms, 100, 60000)) { if (error) *error = "jitter.timeout: must be 100ms~60000ms"; return false; } cfg->jitter.timeout_ms.store(ms); return true; }
        if (field == "window") { uint32_t w; if (!parseUint(value, &w) || !checkRange(w, 2, 1000)) { if (error) *error = "jitter.window: must be 2~1000"; return false; } cfg->jitter.window_size.store(w); return true; }
    }
    if (mon == "rssi") {
        if (field == "enabled") { bool b; if (!parseBool(value, &b)) { if (error) *error = "rssi.enabled: invalid bool"; return false; } cfg->rssi.enabled.store(b); return true; }
        if (field == "interval") { uint32_t ms; if (!parseDurationMs(value, &ms) || !checkRange(ms, 1000, 600000)) { if (error) *error = "rssi.interval: must be 1000ms~600000ms"; return false; } cfg->rssi.interval_ms.store(ms); return true; }
    }
    if (mon == "tcp_loss") {
        if (field == "enabled") { bool b; if (!parseBool(value, &b)) { if (error) *error = "tcp_loss.enabled: invalid bool"; return false; } cfg->tcp_loss.enabled.store(b); return true; }
        if (field == "interval") { uint32_t ms; if (!parseDurationMs(value, &ms) || !checkRange(ms, 1000, 600000)) { if (error) *error = "tcp_loss.interval: must be 1000ms~600000ms"; return false; } cfg->tcp_loss.interval_ms.store(ms); return true; }
    }
    if (mon == "traffic") {
        if (field == "enabled") { bool b; if (!parseBool(value, &b)) { if (error) *error = "traffic.enabled: invalid bool"; return false; } cfg->traffic.enabled.store(b); return true; }
        if (field == "interval") { uint32_t ms; if (!parseDurationMs(value, &ms) || !checkRange(ms, 1000, 600000)) { if (error) *error = "traffic.interval: must be 1000ms~600000ms"; return false; } cfg->traffic.interval_ms.store(ms); return true; }
    }
    if (mon == "quality") {
        if (field == "enabled") { bool b; if (!parseBool(value, &b)) { if (error) *error = "quality.enabled: invalid bool"; return false; } cfg->quality.enabled.store(b); return true; }
        if (field == "interval") { uint32_t ms; if (!parseDurationMs(value, &ms) || !checkRange(ms, 1000, 600000)) { if (error) *error = "quality.interval: must be 1000ms~600000ms"; return false; } cfg->quality.interval_ms.store(ms); return true; }
    }
    if (mon == "bluetooth") {
        if (field == "enabled") { bool b; if (!parseBool(value, &b)) { if (error) *error = "bluetooth.enabled: invalid bool"; return false; } cfg->bluetooth.enabled.store(b); return true; }
        if (field == "interval") { uint32_t ms; if (!parseDurationMs(value, &ms) || !checkRange(ms, 1000, 60000)) { if (error) *error = "bluetooth.interval: must be 1000ms~60000ms"; return false; } cfg->bluetooth.interval_ms.store(ms); return true; }
        if (field == "bpf_obj") { cfg->bluetooth.bpf_obj.set(trim(value)); return true; }
    }
    if (mon == "dns") {
        if (field == "enabled") { bool b; if (!parseBool(value, &b)) { if (error) *error = "dns.enabled: invalid bool"; return false; } cfg->dns.enabled.store(b); return true; }
        if (field == "bpf_obj") { cfg->dns.bpf_obj.set(trim(value)); return true; }
        if (field == "interval") { uint32_t ms; if (!parseDurationMs(value, &ms) || !checkRange(ms, 1000, 600000)) { if (error) *error = "dns.interval: must be 1000ms~600000ms"; return false; } cfg->dns.interval_ms.store(ms); return true; }
    }
    if (mon == "wifi_loss") {
        if (field == "enabled") { bool b; if (!parseBool(value, &b)) { if (error) *error = "wifi_loss.enabled: invalid bool"; return false; } cfg->wifi_loss.enabled.store(b); return true; }
        if (field == "bpf_obj") { cfg->wifi_loss.bpf_obj.set(trim(value)); return true; }
        if (field == "interval") { uint32_t ms; if (!parseDurationMs(value, &ms) || !checkRange(ms, 1000, 600000)) { if (error) *error = "wifi_loss.interval: must be 1000ms~600000ms"; return false; } cfg->wifi_loss.interval_ms.store(ms); return true; }
    }
    if (mon == "http_latency") {
        if (field == "enabled") { bool b; if (!parseBool(value, &b)) { if (error) *error = "http_latency.enabled: invalid bool"; return false; } cfg->http_latency.enabled.store(b); return true; }
        if (field == "bpf_obj") { cfg->http_latency.bpf_obj.set(trim(value)); return true; }
        if (field == "interval") { uint32_t ms; if (!parseDurationMs(value, &ms) || !checkRange(ms, 1000, 600000)) { if (error) *error = "http_latency.interval: must be 1000ms~600000ms"; return false; } cfg->http_latency.interval_ms.store(ms); return true; }
    }
    if (mon == "process_profiler") {
        if (field == "enabled") { bool b; if (!parseBool(value, &b)) { if (error) *error = "process_profiler.enabled: invalid bool"; return false; } cfg->process_profiler.enabled.store(b); return true; }
        if (field == "bpf_obj") { cfg->process_profiler.bpf_obj.set(trim(value)); return true; }
        if (field == "interval") { uint32_t ms; if (!parseDurationMs(value, &ms) || !checkRange(ms, 1000, 600000)) { if (error) *error = "process_profiler.interval: must be 1000ms~600000ms"; return false; } cfg->process_profiler.interval_ms.store(ms); return true; }
    }
    if (mon == "tcp_retrans") {
        if (field == "enabled") { bool b; if (!parseBool(value, &b)) { if (error) *error = "tcp_retrans.enabled: invalid bool"; return false; } cfg->tcp_retrans.enabled.store(b); return true; }
        if (field == "bpf_obj") { cfg->tcp_retrans.bpf_obj.set(trim(value)); return true; }
        if (field == "interval") { uint32_t ms; if (!parseDurationMs(value, &ms) || !checkRange(ms, 1000, 600000)) { if (error) *error = "tcp_retrans.interval: must be 1000ms~600000ms"; return false; } cfg->tcp_retrans.interval_ms.store(ms); return true; }
    }
    if (mon == "tcp_conn") {
        if (field == "enabled") { bool b; if (!parseBool(value, &b)) { if (error) *error = "tcp_conn.enabled: invalid bool"; return false; } cfg->tcp_conn.enabled.store(b); return true; }
        if (field == "bpf_obj") { cfg->tcp_conn.bpf_obj.set(trim(value)); return true; }
        if (field == "interval") { uint32_t ms; if (!parseDurationMs(value, &ms) || !checkRange(ms, 1000, 600000)) { if (error) *error = "tcp_conn.interval: must be 1000ms~600000ms"; return false; } cfg->tcp_conn.interval_ms.store(ms); return true; }
    }
    if (mon == "server") {
        if (field == "dbus_name") { cfg->dbus_name.set(trim(value)); return true; }
        if (field == "data_dir") { cfg->data_dir.set(trim(value)); return true; }
        if (field == "log_level") { cfg->log_level.set(trim(value)); return true; }
    }

    if (error) *error = "unknown monitor or field: " + key;
    return false;
}

std::string serializeMonitorJson(const WeakNetConfig& cfg, const std::string& monitor,
                                 std::string* error) {
    // 未知 monitor 直接报错（支持 "all" + 13 个监控器 + "server"）
    static const std::set<std::string> valid = {
        "all", "server", "rtt", "jitter", "rssi", "tcp_loss", "traffic", "quality",
        "bluetooth", "dns", "wifi_loss", "http_latency", "process_profiler",
        "tcp_retrans", "tcp_conn"
    };
    if (valid.find(monitor) == valid.end()) {
        if (error) *error = "unknown monitor: " + monitor;
        return "";
    }

    std::ostringstream json;
    json << '{';

    auto writeBool = [&](const char* name, bool v) {
        json << '"' << name << "\":" << (v ? "true" : "false") << ',';
    };
    auto writeUint = [&](const char* name, uint32_t v) {
        json << '"' << name << "\":" << v << ',';
    };
    auto writeString = [&](const char* name, const std::string& v) {
        json << '"' << name << "\":\"" << weaknet_utils::escapeJsonString(v) << "\",";
    };

    if (monitor == "all" || monitor == "server") {
        json << "\"server\":{";
        writeString("dbus_name", cfg.dbus_name.get());
        writeString("data_dir", cfg.data_dir.get());
        writeString("log_level", cfg.log_level.get());
        json.seekp(-1, std::ios_base::cur); json << "},";
    }
    if (monitor == "all" || monitor == "rtt") {
        json << "\"rtt\":{";
        writeBool("enabled", cfg.rtt.enabled.load());
        writeString("target", cfg.rtt.target.get());
        writeUint("interval_ms", cfg.rtt.interval_ms.load());
        writeUint("timeout_ms", cfg.rtt.timeout_ms.load());
        json.seekp(-1, std::ios_base::cur); json << "},";
    }
    if (monitor == "all" || monitor == "jitter") {
        json << "\"jitter\":{";
        writeBool("enabled", cfg.jitter.enabled.load());
        writeString("target", cfg.jitter.target.get());
        writeUint("interval_ms", cfg.jitter.interval_ms.load());
        writeUint("timeout_ms", cfg.jitter.timeout_ms.load());
        writeUint("window_size", cfg.jitter.window_size.load());
        json.seekp(-1, std::ios_base::cur); json << "},";
    }
    if (monitor == "all" || monitor == "rssi") {
        json << "\"rssi\":{";
        writeBool("enabled", cfg.rssi.enabled.load());
        writeUint("interval_ms", cfg.rssi.interval_ms.load());
        json.seekp(-1, std::ios_base::cur); json << "},";
    }
    if (monitor == "all" || monitor == "tcp_loss") {
        json << "\"tcp_loss\":{";
        writeBool("enabled", cfg.tcp_loss.enabled.load());
        writeUint("interval_ms", cfg.tcp_loss.interval_ms.load());
        json.seekp(-1, std::ios_base::cur); json << "},";
    }
    if (monitor == "all" || monitor == "traffic") {
        json << "\"traffic\":{";
        writeBool("enabled", cfg.traffic.enabled.load());
        writeUint("interval_ms", cfg.traffic.interval_ms.load());
        json.seekp(-1, std::ios_base::cur); json << "},";
    }
    if (monitor == "all" || monitor == "quality") {
        json << "\"quality\":{";
        writeBool("enabled", cfg.quality.enabled.load());
        writeUint("interval_ms", cfg.quality.interval_ms.load());
        json.seekp(-1, std::ios_base::cur); json << "},";
    }
    if (monitor == "all" || monitor == "bluetooth") {
        json << "\"bluetooth\":{";
        writeBool("enabled", cfg.bluetooth.enabled.load());
        writeUint("interval_ms", cfg.bluetooth.interval_ms.load());
        writeString("bpf_obj", cfg.bluetooth.bpf_obj.get());
        json.seekp(-1, std::ios_base::cur); json << "},";
    }
    if (monitor == "all" || monitor == "dns") {
        json << "\"dns\":{";
        writeBool("enabled", cfg.dns.enabled.load());
        writeString("bpf_obj", cfg.dns.bpf_obj.get());
        writeUint("interval_ms", cfg.dns.interval_ms.load());
        json.seekp(-1, std::ios_base::cur); json << "},";
    }
    if (monitor == "all" || monitor == "wifi_loss") {
        json << "\"wifi_loss\":{";
        writeBool("enabled", cfg.wifi_loss.enabled.load());
        writeString("bpf_obj", cfg.wifi_loss.bpf_obj.get());
        writeUint("interval_ms", cfg.wifi_loss.interval_ms.load());
        json.seekp(-1, std::ios_base::cur); json << "},";
    }
    if (monitor == "all" || monitor == "http_latency") {
        json << "\"http_latency\":{";
        writeBool("enabled", cfg.http_latency.enabled.load());
        writeString("bpf_obj", cfg.http_latency.bpf_obj.get());
        writeUint("interval_ms", cfg.http_latency.interval_ms.load());
        json.seekp(-1, std::ios_base::cur); json << "},";
    }
    if (monitor == "all" || monitor == "process_profiler") {
        json << "\"process_profiler\":{";
        writeBool("enabled", cfg.process_profiler.enabled.load());
        writeString("bpf_obj", cfg.process_profiler.bpf_obj.get());
        writeUint("interval_ms", cfg.process_profiler.interval_ms.load());
        json.seekp(-1, std::ios_base::cur); json << "},";
    }
    if (monitor == "all" || monitor == "tcp_retrans") {
        json << "\"tcp_retrans\":{";
        writeBool("enabled", cfg.tcp_retrans.enabled.load());
        writeString("bpf_obj", cfg.tcp_retrans.bpf_obj.get());
        writeUint("interval_ms", cfg.tcp_retrans.interval_ms.load());
        json.seekp(-1, std::ios_base::cur); json << "},";
    }
    if (monitor == "all" || monitor == "tcp_conn") {
        json << "\"tcp_conn\":{";
        writeBool("enabled", cfg.tcp_conn.enabled.load());
        writeString("bpf_obj", cfg.tcp_conn.bpf_obj.get());
        writeUint("interval_ms", cfg.tcp_conn.interval_ms.load());
        json.seekp(-1, std::ios_base::cur); json << "},";
    }

    // 去掉最后一个逗号
    auto s = json.str();
    if (!s.empty() && s.back() == ',') s.pop_back();
    s.push_back('}');
    return s;
}

}  // namespace weaknet_dbus
