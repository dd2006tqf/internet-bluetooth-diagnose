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
#include <cstdio>
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

/**
 * @brief 剥离值两端成对的引号（'...' 或 "..."）
 *
 * 为什么需要：本解析器是 YAML 子集，但配置书写者自然会按 YAML 习惯加引号，
 * 文档中的格式示例也写作 "id|host|port"。此前引号会被当成值的一部分，
 * 造成"配置看起来对、行为却错"的静默故障：
 *
 *   实测（开发板，2026-09-13）：
 *     portal_path: "/success.txt"
 *     → 实际值 "/success.txt"（含字面引号）
 *     → HTTP 请求路径畸形 → detectportal.firefox.com 返回 404、
 *       captive.apple.com 返回 400
 *     → Portal oracle 判定为内容不匹配，能力永远无法建立
 *
 * 注意 stripInlineComment 已先行处理注释，故此处只需处理成对引号；
 * 只有首尾同引号且长度 >= 2 时才剥离，避免误伤 'a 这类不完整输入。
 */
std::string stripQuotes(const std::string& s) {
    if (s.size() >= 2) {
        const char q = s.front();
        if ((q == '"' || q == '\'') && s.back() == q) {
            return s.substr(1, s.size() - 2);
        }
    }
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
        if (field == "interval" || field == "interval_ms") return setDurationField(cfg->rtt.interval_ms, val, error);
        if (field == "timeout" || field == "timeout_ms") return setDurationField(cfg->rtt.timeout_ms, val, error);
        *error = "rtt: unknown field '" + field + "'";
        return false;
    }
    if (mon == "jitter") {
        if (field == "enabled") return setBoolField(cfg->jitter.enabled, val, error);
        if (field == "target") { cfg->jitter.target.set(trim(val)); return true; }
        if (field == "interval" || field == "interval_ms") return setDurationField(cfg->jitter.interval_ms, val, error);
        if (field == "timeout" || field == "timeout_ms") return setDurationField(cfg->jitter.timeout_ms, val, error);
        if (field == "window" || field == "window_size") return setDurationField(cfg->jitter.window_size, val, error);
        *error = "jitter: unknown field '" + field + "'";
        return false;
    }
    if (mon == "rssi") {
        if (field == "enabled") return setBoolField(cfg->rssi.enabled, val, error);
        if (field == "interval" || field == "interval_ms") return setDurationField(cfg->rssi.interval_ms, val, error);
        *error = "rssi: unknown field '" + field + "'";
        return false;
    }
    if (mon == "tcp_loss") {
        if (field == "enabled") return setBoolField(cfg->tcp_loss.enabled, val, error);
        if (field == "interval" || field == "interval_ms") return setDurationField(cfg->tcp_loss.interval_ms, val, error);
        *error = "tcp_loss: unknown field '" + field + "'";
        return false;
    }
    if (mon == "traffic") {
        if (field == "enabled") return setBoolField(cfg->traffic.enabled, val, error);
        if (field == "interval" || field == "interval_ms") return setDurationField(cfg->traffic.interval_ms, val, error);
        *error = "traffic: unknown field '" + field + "'";
        return false;
    }
    if (mon == "quality") {
        if (field == "enabled") return setBoolField(cfg->quality.enabled, val, error);
        if (field == "interval" || field == "interval_ms") return setDurationField(cfg->quality.interval_ms, val, error);
        *error = "quality: unknown field '" + field + "'";
        return false;
    }
    if (mon == "bluetooth") {
        if (field == "enabled") return setBoolField(cfg->bluetooth.enabled, val, error);
        if (field == "interval" || field == "interval_ms") return setDurationField(cfg->bluetooth.interval_ms, val, error);
        if (field == "bpf_obj") { cfg->bluetooth.bpf_obj.set(trim(val)); return true; }
        *error = "bluetooth: unknown field '" + field + "'";
        return false;
    }
    if (mon == "dns") {
        if (field == "enabled") return setBoolField(cfg->dns.enabled, val, error);
        if (field == "bpf_obj") { cfg->dns.bpf_obj.set(trim(val)); return true; }
        if (field == "interval" || field == "interval_ms") return setDurationField(cfg->dns.interval_ms, val, error);
        if (field == "capture_pages") {
            uint32_t pages = 0;
            if (!parseUint(val, &pages) || pages < 1 || pages > 256) {
                *error = "dns.capture_pages: must be 1~256";
                return false;
            }
            cfg->dns.capture_pages.store(pages);
            return true;
        }
        if (field == "assessment_profile") {
            const std::string v = trim(val);
            if (v != "NETWORK_ONLY" && v != "INTERNET_ACCESS") {
                *error = "dns.assessment_profile: must be NETWORK_ONLY or INTERNET_ACCESS";
                return false;
            }
            cfg->dns.assessment_profile.set(v);
            return true;
        }
        *error = "dns: unknown field '" + field + "'";
        return false;
    }
    if (mon == "active_probe") {
        if (field == "enabled") return setBoolField(cfg->active_probe.enabled, val, error);
        if (field == "interval" || field == "interval_sec" || field == "interval_ms") return setDurationField(cfg->active_probe.interval_ms, val, error);
        if (field == "timeout" || field == "timeout_sec" || field == "timeout_ms") return setDurationField(cfg->active_probe.timeout_ms, val, error);
        if (field == "targets") { cfg->active_probe.targets.set(trim(val)); return true; }
        if (field == "https_enabled") return setBoolField(cfg->active_probe.https_enabled, val, error);
        if (field == "portal_check_enabled") return setBoolField(cfg->active_probe.portal_check_enabled, val, error);
        if (field == "portal_targets") { cfg->active_probe.portal_targets.set(trim(val)); return true; }
        if (field == "portal_path") { cfg->active_probe.portal_path.set(trim(val)); return true; }
        if (field == "portal_expect_body") { cfg->active_probe.portal_expect_body.set(trim(val)); return true; }
        *error = "active_probe: unknown field '" + field + "'";
        return false;
    }
    if (mon == "tcp_connect") {
        if (field == "enabled") return setBoolField(cfg->tcp_connect.enabled, val, error);
        if (field == "bpf_obj") { cfg->tcp_connect.bpf_obj.set(trim(val)); return true; }
        if (field == "interval" || field == "interval_ms") return setDurationField(cfg->tcp_connect.interval_ms, val, error);
        if (field == "capture_pages") {
            uint32_t pages = 0;
            if (!parseUint(val, &pages) || pages < 1 || pages > 256) {
                *error = "tcp_connect.capture_pages: must be 1~256";
                return false;
            }
            cfg->tcp_connect.capture_pages.store(pages);
            return true;
        }
        *error = "tcp_connect: unknown field '" + field + "'";
        return false;
    }
    if (mon == "wifi_loss") {
        if (field == "enabled") return setBoolField(cfg->wifi_loss.enabled, val, error);
        if (field == "bpf_obj") { cfg->wifi_loss.bpf_obj.set(trim(val)); return true; }
        if (field == "interval" || field == "interval_ms") return setDurationField(cfg->wifi_loss.interval_ms, val, error);
        *error = "wifi_loss: unknown field '" + field + "'";
        return false;
    }
    if (mon == "http_latency") {
        if (field == "enabled") return setBoolField(cfg->http_latency.enabled, val, error);
        if (field == "bpf_obj") { cfg->http_latency.bpf_obj.set(trim(val)); return true; }
        if (field == "interval" || field == "interval_ms") return setDurationField(cfg->http_latency.interval_ms, val, error);
        *error = "http_latency: unknown field '" + field + "'";
        return false;
    }
    if (mon == "process_profiler") {
        if (field == "enabled") return setBoolField(cfg->process_profiler.enabled, val, error);
        if (field == "bpf_obj") { cfg->process_profiler.bpf_obj.set(trim(val)); return true; }
        if (field == "interval" || field == "interval_ms") return setDurationField(cfg->process_profiler.interval_ms, val, error);
        *error = "process_profiler: unknown field '" + field + "'";
        return false;
    }
    if (mon == "tcp_retrans") {
        if (field == "enabled") return setBoolField(cfg->tcp_retrans.enabled, val, error);
        if (field == "bpf_obj") { cfg->tcp_retrans.bpf_obj.set(trim(val)); return true; }
        if (field == "interval" || field == "interval_ms") return setDurationField(cfg->tcp_retrans.interval_ms, val, error);
        *error = "tcp_retrans: unknown field '" + field + "'";
        return false;
    }
    if (mon == "tcp_conn") {
        if (field == "enabled") return setBoolField(cfg->tcp_conn.enabled, val, error);
        if (field == "bpf_obj") { cfg->tcp_conn.bpf_obj.set(trim(val)); return true; }
        if (field == "interval" || field == "interval_ms") return setDurationField(cfg->tcp_conn.interval_ms, val, error);
        *error = "tcp_conn: unknown field '" + field + "'";
        return false;
    }
    if (mon == "skb_drop") {
        if (field == "enabled") return setBoolField(cfg->skb_drop.enabled, val, error);
        if (field == "bpf_obj") { cfg->skb_drop.bpf_obj.set(trim(val)); return true; }
        if (field == "interval" || field == "interval_ms") return setDurationField(cfg->skb_drop.interval_ms, val, error);
        *error = "skb_drop: unknown field '" + field + "'";
        return false;
    }
    *error = "unknown monitor: '" + mon + "'";
    return false;
}

// ---- 服务端 section 字段分发 ----

// ---- 边缘遥测上报 section 字段分发 ----

bool applyEdgeField(WeakNetConfig* cfg, const std::string& field,
                    const std::string& val, std::string* error) {
    if (field == "enabled") return setBoolField(cfg->edge.enabled, val, error);
    if (field == "url") { cfg->edge.url.set(trim(val)); return true; }
    if (field == "tenant") { cfg->edge.tenant.set(trim(val)); return true; }
    if (field == "device_id") { cfg->edge.device_id.set(trim(val)); return true; }
    if (field == "token") { cfg->edge.token.set(trim(val)); return true; }
    if (field == "private_key_path") { cfg->edge.private_key_path.set(trim(val)); return true; }
    if (field == "key_id") { cfg->edge.key_id.set(trim(val)); return true; }
    if (field == "interval" || field == "interval_ms") {
        uint32_t ms;
        if (!parseDurationMs(val, &ms) || ms < 1000 || ms > 3600000) {
            *error = "edge.interval: must be 1000ms~3600000ms";
            return false;
        }
        cfg->edge.interval_ms.store(ms);
        return true;
    }
    if (field == "timeout" || field == "timeout_ms") {
        uint32_t ms;
        if (!parseDurationMs(val, &ms) || ms < 100 || ms > 120000) {
            *error = "edge.timeout: must be 100ms~120000ms";
            return false;
        }
        cfg->edge.timeout_ms.store(ms);
        return true;
    }
    *error = "edge: unknown field '" + field + "'";
    return false;
}

bool applyServerField(WeakNetConfig* cfg, const std::string& field,
                      const std::string& val, std::string* error) {
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
        // 值先剥引号再 trim：stripQuotes 处理成对引号（见其文档注释），
        // 保留引号内的空白语义，故顺序是 trim → stripQuotes → trim。
        std::string value = trim(stripQuotes(trim(trimmed.substr(colon + 1))));

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
        } else if (section == "edge") {
            if (!applyEdgeField(out, key, value, error)) {
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

bool getMonitorEnabled(const WeakNetConfig& cfg, const std::string& monitor, bool* enabled) {
    if (!enabled) return false;
    if (monitor == "iface" || monitor == "using_iface") *enabled = true;
    else if (monitor == "rtt") *enabled = cfg.rtt.enabled.load();
    else if (monitor == "jitter") *enabled = cfg.jitter.enabled.load();
    else if (monitor == "rssi") *enabled = cfg.rssi.enabled.load();
    else if (monitor == "tcp_loss") *enabled = cfg.tcp_loss.enabled.load();
    else if (monitor == "traffic") *enabled = cfg.traffic.enabled.load();
    else if (monitor == "quality") *enabled = cfg.quality.enabled.load();
    else if (monitor == "bluetooth") *enabled = cfg.bluetooth.enabled.load();
    else if (monitor == "dns") *enabled = cfg.dns.enabled.load();
    else if (monitor == "tcp_connect") *enabled = cfg.tcp_connect.enabled.load();
    else if (monitor == "active_probe") *enabled = cfg.active_probe.enabled.load();
    else if (monitor == "wifi_loss") *enabled = cfg.wifi_loss.enabled.load();
    else if (monitor == "http_latency") *enabled = cfg.http_latency.enabled.load();
    else if (monitor == "process_profiler") *enabled = cfg.process_profiler.enabled.load();
    else if (monitor == "tcp_retrans") *enabled = cfg.tcp_retrans.enabled.load();
    else if (monitor == "tcp_conn") *enabled = cfg.tcp_conn.enabled.load();
    else if (monitor == "skb_drop") *enabled = cfg.skb_drop.enabled.load();
    else return false;
    return true;
}

bool setMonitorEnabled(WeakNetConfig* cfg, const std::string& monitor, bool enabled) {
    if (!cfg) return false;
    if (monitor == "iface" || monitor == "using_iface") return true;
    if (monitor == "rtt") cfg->rtt.enabled.store(enabled);
    else if (monitor == "jitter") cfg->jitter.enabled.store(enabled);
    else if (monitor == "rssi") cfg->rssi.enabled.store(enabled);
    else if (monitor == "tcp_loss") cfg->tcp_loss.enabled.store(enabled);
    else if (monitor == "traffic") cfg->traffic.enabled.store(enabled);
    else if (monitor == "quality") cfg->quality.enabled.store(enabled);
    else if (monitor == "bluetooth") cfg->bluetooth.enabled.store(enabled);
    else if (monitor == "dns") cfg->dns.enabled.store(enabled);
    else if (monitor == "tcp_connect") cfg->tcp_connect.enabled.store(enabled);
    else if (monitor == "active_probe") cfg->active_probe.enabled.store(enabled);
    else if (monitor == "wifi_loss") cfg->wifi_loss.enabled.store(enabled);
    else if (monitor == "http_latency") cfg->http_latency.enabled.store(enabled);
    else if (monitor == "process_profiler") cfg->process_profiler.enabled.store(enabled);
    else if (monitor == "tcp_retrans") cfg->tcp_retrans.enabled.store(enabled);
    else if (monitor == "tcp_conn") cfg->tcp_conn.enabled.store(enabled);
    else if (monitor == "skb_drop") cfg->skb_drop.enabled.store(enabled);
    else return false;
    return true;
}

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
        if (field == "interval" || field == "interval_ms") { uint32_t ms; if (!parseDurationMs(value, &ms) || !checkRange(ms, 100, 600000)) { if (error) *error = "rtt.interval: must be 100ms~600000ms"; return false; } cfg->rtt.interval_ms.store(ms); return true; }
        if (field == "timeout" || field == "timeout_ms") { uint32_t ms; if (!parseDurationMs(value, &ms) || !checkRange(ms, 100, 60000)) { if (error) *error = "rtt.timeout: must be 100ms~60000ms"; return false; } cfg->rtt.timeout_ms.store(ms); return true; }
    }
    if (mon == "jitter") {
        if (field == "enabled") { bool b; if (!parseBool(value, &b)) { if (error) *error = "jitter.enabled: invalid bool"; return false; } cfg->jitter.enabled.store(b); return true; }
        if (field == "target") { if (!isValidIPv4(trim(value))) { if (error) *error = "jitter.target: invalid IPv4"; return false; } cfg->jitter.target.set(trim(value)); return true; }
        if (field == "interval" || field == "interval_ms") { uint32_t ms; if (!parseDurationMs(value, &ms) || !checkRange(ms, 100, 600000)) { if (error) *error = "jitter.interval: must be 100ms~600000ms"; return false; } cfg->jitter.interval_ms.store(ms); return true; }
        if (field == "timeout" || field == "timeout_ms") { uint32_t ms; if (!parseDurationMs(value, &ms) || !checkRange(ms, 100, 60000)) { if (error) *error = "jitter.timeout: must be 100ms~60000ms"; return false; } cfg->jitter.timeout_ms.store(ms); return true; }
        if (field == "window" || field == "window_size") { uint32_t w; if (!parseUint(value, &w) || !checkRange(w, 2, 1000)) { if (error) *error = "jitter.window: must be 2~1000"; return false; } cfg->jitter.window_size.store(w); return true; }
    }
    if (mon == "rssi") {
        if (field == "enabled") { bool b; if (!parseBool(value, &b)) { if (error) *error = "rssi.enabled: invalid bool"; return false; } cfg->rssi.enabled.store(b); return true; }
        if (field == "interval" || field == "interval_ms") { uint32_t ms; if (!parseDurationMs(value, &ms) || !checkRange(ms, 1000, 600000)) { if (error) *error = "rssi.interval: must be 1000ms~600000ms"; return false; } cfg->rssi.interval_ms.store(ms); return true; }
    }
    if (mon == "tcp_loss") {
        if (field == "enabled") { bool b; if (!parseBool(value, &b)) { if (error) *error = "tcp_loss.enabled: invalid bool"; return false; } cfg->tcp_loss.enabled.store(b); return true; }
        if (field == "interval" || field == "interval_ms") { uint32_t ms; if (!parseDurationMs(value, &ms) || !checkRange(ms, 1000, 600000)) { if (error) *error = "tcp_loss.interval: must be 1000ms~600000ms"; return false; } cfg->tcp_loss.interval_ms.store(ms); return true; }
    }
    if (mon == "traffic") {
        if (field == "enabled") { bool b; if (!parseBool(value, &b)) { if (error) *error = "traffic.enabled: invalid bool"; return false; } cfg->traffic.enabled.store(b); return true; }
        if (field == "interval" || field == "interval_ms") { uint32_t ms; if (!parseDurationMs(value, &ms) || !checkRange(ms, 1000, 600000)) { if (error) *error = "traffic.interval: must be 1000ms~600000ms"; return false; } cfg->traffic.interval_ms.store(ms); return true; }
    }
    if (mon == "quality") {
        if (field == "enabled") { bool b; if (!parseBool(value, &b)) { if (error) *error = "quality.enabled: invalid bool"; return false; } cfg->quality.enabled.store(b); return true; }
        if (field == "interval" || field == "interval_ms") { uint32_t ms; if (!parseDurationMs(value, &ms) || !checkRange(ms, 1000, 600000)) { if (error) *error = "quality.interval: must be 1000ms~600000ms"; return false; } cfg->quality.interval_ms.store(ms); return true; }
    }
    if (mon == "bluetooth") {
        if (field == "enabled") { bool b; if (!parseBool(value, &b)) { if (error) *error = "bluetooth.enabled: invalid bool"; return false; } cfg->bluetooth.enabled.store(b); return true; }
        if (field == "interval" || field == "interval_ms") { uint32_t ms; if (!parseDurationMs(value, &ms) || !checkRange(ms, 1000, 60000)) { if (error) *error = "bluetooth.interval: must be 1000ms~60000ms"; return false; } cfg->bluetooth.interval_ms.store(ms); return true; }
        if (field == "bpf_obj") { cfg->bluetooth.bpf_obj.set(trim(value)); return true; }
    }
    if (mon == "dns") {
        if (field == "enabled") { bool b; if (!parseBool(value, &b)) { if (error) *error = "dns.enabled: invalid bool"; return false; } cfg->dns.enabled.store(b); return true; }
        if (field == "bpf_obj") { cfg->dns.bpf_obj.set(trim(value)); return true; }
        if (field == "interval" || field == "interval_ms") { uint32_t ms; if (!parseDurationMs(value, &ms) || !checkRange(ms, 1000, 600000)) { if (error) *error = "dns.interval: must be 1000ms~600000ms"; return false; } cfg->dns.interval_ms.store(ms); return true; }
        if (field == "capture_pages") { uint32_t pages; if (!parseUint(value, &pages) || !checkRange(pages, 1, 256)) { if (error) *error = "dns.capture_pages: must be 1~256"; return false; } cfg->dns.capture_pages.store(pages); return true; }
        if (field == "assessment_profile") {
            if (value != "NETWORK_ONLY" && value != "INTERNET_ACCESS") { if (error) *error = "dns.assessment_profile: must be NETWORK_ONLY or INTERNET_ACCESS"; return false; }
            cfg->dns.assessment_profile.set(value); return true;
        }
    }
    if (mon == "active_probe") {
        if (field == "enabled") { bool b; if (!parseBool(value, &b)) { if (error) *error = "active_probe.enabled: invalid bool"; return false; } cfg->active_probe.enabled.store(b); return true; }
        if (field == "interval" || field == "interval_sec" || field == "interval_ms") { uint32_t ms; if (!parseDurationMs(value, &ms) || !checkRange(ms, 5000, 3600000)) { if (error) *error = "active_probe.interval: must be 5000ms~3600000ms"; return false; } cfg->active_probe.interval_ms.store(ms); return true; }
        if (field == "timeout" || field == "timeout_sec" || field == "timeout_ms") { uint32_t ms; if (!parseDurationMs(value, &ms) || !checkRange(ms, 500, 30000)) { if (error) *error = "active_probe.timeout: must be 500ms~30000ms"; return false; } cfg->active_probe.timeout_ms.store(ms); return true; }
        if (field == "targets") { cfg->active_probe.targets.set(trim(value)); return true; }
        if (field == "https_enabled") { bool b; if (!parseBool(value, &b)) { if (error) *error = "active_probe.https_enabled: invalid bool"; return false; } cfg->active_probe.https_enabled.store(b); return true; }
        if (field == "portal_check_enabled") { bool b; if (!parseBool(value, &b)) { if (error) *error = "active_probe.portal_check_enabled: invalid bool"; return false; } cfg->active_probe.portal_check_enabled.store(b); return true; }
        if (field == "portal_targets") { cfg->active_probe.portal_targets.set(trim(value)); return true; }
        if (field == "portal_path") { cfg->active_probe.portal_path.set(trim(value)); return true; }
        if (field == "portal_expect_body") { cfg->active_probe.portal_expect_body.set(trim(value)); return true; }
    }
    if (mon == "tcp_connect") {
        if (field == "enabled") { bool b; if (!parseBool(value, &b)) { if (error) *error = "tcp_connect.enabled: invalid bool"; return false; } cfg->tcp_connect.enabled.store(b); return true; }
        if (field == "bpf_obj") { cfg->tcp_connect.bpf_obj.set(trim(value)); return true; }
        if (field == "interval" || field == "interval_ms") { uint32_t ms; if (!parseDurationMs(value, &ms) || !checkRange(ms, 1000, 600000)) { if (error) *error = "tcp_connect.interval: must be 1000ms~600000ms"; return false; } cfg->tcp_connect.interval_ms.store(ms); return true; }
        if (field == "capture_pages") { uint32_t pages; if (!parseUint(value, &pages) || !checkRange(pages, 1, 256)) { if (error) *error = "tcp_connect.capture_pages: must be 1~256"; return false; } cfg->tcp_connect.capture_pages.store(pages); return true; }
    }
    if (mon == "wifi_loss") {
        if (field == "enabled") { bool b; if (!parseBool(value, &b)) { if (error) *error = "wifi_loss.enabled: invalid bool"; return false; } cfg->wifi_loss.enabled.store(b); return true; }
        if (field == "bpf_obj") { cfg->wifi_loss.bpf_obj.set(trim(value)); return true; }
        if (field == "interval" || field == "interval_ms") { uint32_t ms; if (!parseDurationMs(value, &ms) || !checkRange(ms, 1000, 600000)) { if (error) *error = "wifi_loss.interval: must be 1000ms~600000ms"; return false; } cfg->wifi_loss.interval_ms.store(ms); return true; }
    }
    if (mon == "http_latency") {
        if (field == "enabled") { bool b; if (!parseBool(value, &b)) { if (error) *error = "http_latency.enabled: invalid bool"; return false; } cfg->http_latency.enabled.store(b); return true; }
        if (field == "bpf_obj") { cfg->http_latency.bpf_obj.set(trim(value)); return true; }
        if (field == "interval" || field == "interval_ms") { uint32_t ms; if (!parseDurationMs(value, &ms) || !checkRange(ms, 1000, 600000)) { if (error) *error = "http_latency.interval: must be 1000ms~600000ms"; return false; } cfg->http_latency.interval_ms.store(ms); return true; }
    }
    if (mon == "process_profiler") {
        if (field == "enabled") { bool b; if (!parseBool(value, &b)) { if (error) *error = "process_profiler.enabled: invalid bool"; return false; } cfg->process_profiler.enabled.store(b); return true; }
        if (field == "bpf_obj") { cfg->process_profiler.bpf_obj.set(trim(value)); return true; }
        if (field == "interval" || field == "interval_ms") { uint32_t ms; if (!parseDurationMs(value, &ms) || !checkRange(ms, 1000, 600000)) { if (error) *error = "process_profiler.interval: must be 1000ms~600000ms"; return false; } cfg->process_profiler.interval_ms.store(ms); return true; }
    }
    if (mon == "tcp_retrans") {
        if (field == "enabled") { bool b; if (!parseBool(value, &b)) { if (error) *error = "tcp_retrans.enabled: invalid bool"; return false; } cfg->tcp_retrans.enabled.store(b); return true; }
        if (field == "bpf_obj") { cfg->tcp_retrans.bpf_obj.set(trim(value)); return true; }
        if (field == "interval" || field == "interval_ms") { uint32_t ms; if (!parseDurationMs(value, &ms) || !checkRange(ms, 1000, 600000)) { if (error) *error = "tcp_retrans.interval: must be 1000ms~600000ms"; return false; } cfg->tcp_retrans.interval_ms.store(ms); return true; }
    }
    if (mon == "tcp_conn") {
        if (field == "enabled") { bool b; if (!parseBool(value, &b)) { if (error) *error = "tcp_conn.enabled: invalid bool"; return false; } cfg->tcp_conn.enabled.store(b); return true; }
        if (field == "bpf_obj") { cfg->tcp_conn.bpf_obj.set(trim(value)); return true; }
        if (field == "interval" || field == "interval_ms") { uint32_t ms; if (!parseDurationMs(value, &ms) || !checkRange(ms, 1000, 600000)) { if (error) *error = "tcp_conn.interval: must be 1000ms~600000ms"; return false; } cfg->tcp_conn.interval_ms.store(ms); return true; }
    }
    if (mon == "skb_drop") {
        if (field == "enabled") { bool b; if (!parseBool(value, &b)) { if (error) *error = "skb_drop.enabled: invalid bool"; return false; } cfg->skb_drop.enabled.store(b); return true; }
        if (field == "bpf_obj") { cfg->skb_drop.bpf_obj.set(trim(value)); return true; }
        if (field == "interval" || field == "interval_ms") { uint32_t ms; if (!parseDurationMs(value, &ms) || !checkRange(ms, 1000, 600000)) { if (error) *error = "skb_drop.interval: must be 1000ms~600000ms"; return false; } cfg->skb_drop.interval_ms.store(ms); return true; }
    }
    if (mon == "server") {
        if (field == "data_dir") { cfg->data_dir.set(trim(value)); return true; }
        if (field == "log_level") { cfg->log_level.set(trim(value)); return true; }
    }

    // 边缘遥测上报的运行时调参。
    //
    // 刻意**不**暴露 url/token/private_key_path/tenant/device_id 的运行时修改：
    // 这些字段改变的是"设备向谁、以什么身份上报"。允许远程改写等于让一次
    // 配置下发就能把遥测重定向到别处或顶替设备身份，风险远高于调周期。
    // 它们只能通过 /etc/weaknet/config.yaml 在启动时确定。
    if (mon == "edge") {
        if (field == "enabled") {
            bool b; if (!parseBool(value, &b)) { if (error) *error = "edge.enabled: invalid bool"; return false; }
            cfg->edge.enabled.store(b); return true;
        }
        if (field == "interval" || field == "interval_ms") {
            uint32_t ms;
            if (!parseDurationMs(value, &ms) || !checkRange(ms, 1000, 3600000)) {
                if (error) *error = "edge.interval: must be 1000ms~3600000ms";
                return false;
            }
            cfg->edge.interval_ms.store(ms);
            return true;
        }
        if (field == "timeout" || field == "timeout_ms") {
            uint32_t ms;
            if (!parseDurationMs(value, &ms) || !checkRange(ms, 100, 120000)) {
                if (error) *error = "edge.timeout: must be 100ms~120000ms";
                return false;
            }
            cfg->edge.timeout_ms.store(ms);
            return true;
        }
    }

    if (error) *error = "unknown monitor or field: " + key;
    return false;
}

// ============================================================================
// ConfigTransaction 实现
// ============================================================================
//
// 状态机语义见头文件注释。实现要点：
//
//   - prior_values 在 startTrial / extendTrial 时通过 snapshotMonitorParam
//     读出并保存。同 key 第二次叠入时不覆盖，保证 ROLLBACK 还原到最初值。
//   - 持久化用 JSON-ish 的 "key\tvalue\n" 行格式（足够简单，不引入 JSON 库）。
//     用 tmp + rename 原子写入；读不到就当没有 crash recovery。
//   - deadlineExpired 只看时间，不看健康——健康判定由 exporter 读最新
//     snapshot 后调 confirmStable / forceRollback。
//   - last_applied_generation_ 单调递增；startTrial 会拒绝 <= 当前值的
//     generation，阻断云端下发的重放。
// ============================================================================

namespace {

/// prior_values 落盘的极简行格式："key\tvalue\n"。key/value 内禁止 \t\n，
/// 我们序列化时已经保证（serializeMonitorParam 不产生这两字符）。
const char kTxnStateFieldSep = '\t';

bool isTrialableKeyImpl(const std::string& key) {
    // 白名单只放"采样/超时/目标 IP"这类调参键。
    // 不放 identity（edge.url/token/device_id/key_id/tenant/private_key_path）
    // 不放 enabled（开关型参数可能直接关闭监控回路）
    // 不放 bpf_obj（eBPF 程序路径变化需要重启加载，不属于运行时事务）
    // 不放 active_probe.*（探测目标变更可能让探针失联）
    static const std::set<std::string> trialable = {
        "rtt.interval_ms", "rtt.interval", "rtt.timeout_ms", "rtt.timeout", "rtt.target",
        "jitter.interval_ms", "jitter.interval", "jitter.timeout_ms", "jitter.timeout",
        "jitter.window_size", "jitter.window", "jitter.target",
        "rssi.interval_ms", "rssi.interval",
        "tcp_loss.interval_ms", "tcp_loss.interval",
        "traffic.interval_ms", "traffic.interval",
        "quality.interval_ms", "quality.interval",
        "bluetooth.interval_ms", "bluetooth.interval",
        "dns.interval_ms", "dns.interval", "dns.capture_pages",
        "tcp_connect.interval_ms", "tcp_connect.interval", "tcp_connect.capture_pages",
        "wifi_loss.interval_ms", "wifi_loss.interval",
        "http_latency.interval_ms", "http_latency.interval",
        "process_profiler.interval_ms", "process_profiler.interval",
        "tcp_retrans.interval_ms", "tcp_retrans.interval",
        "tcp_conn.interval_ms", "tcp_conn.interval",
        "skb_drop.interval_ms", "skb_drop.interval",
        "edge.interval_ms", "edge.interval", "edge.timeout_ms", "edge.timeout",
    };
    return trialable.find(key) != trialable.end();
}

/// 序列化某个 trialable key 的当前值（用于 prior_values 快照）。
/// 不复用 serializeMonitorJson —— 那个序列化整 monitor，这里要单字段。
bool snapshotMonitorParamImpl(const WeakNetConfig& cfg, const std::string& key,
                              std::string* value_out) {
    if (!value_out) return false;
    std::string mon, field;
    if (!splitMonitorKey(key, &mon, &field)) return false;

    auto to_str_u32 = [](uint32_t v) { return std::to_string(v); };

    if (mon == "rtt") {
        if (field == "interval" || field == "interval_ms") { *value_out = to_str_u32(cfg.rtt.interval_ms.load()); return true; }
        if (field == "timeout" || field == "timeout_ms") { *value_out = to_str_u32(cfg.rtt.timeout_ms.load()); return true; }
        if (field == "target") { *value_out = cfg.rtt.target.get(); return true; }
    }
    if (mon == "jitter") {
        if (field == "interval" || field == "interval_ms") { *value_out = to_str_u32(cfg.jitter.interval_ms.load()); return true; }
        if (field == "timeout" || field == "timeout_ms") { *value_out = to_str_u32(cfg.jitter.timeout_ms.load()); return true; }
        if (field == "window" || field == "window_size") { *value_out = to_str_u32(cfg.jitter.window_size.load()); return true; }
        if (field == "target") { *value_out = cfg.jitter.target.get(); return true; }
    }
    if (mon == "rssi") {
        if (field == "interval" || field == "interval_ms") { *value_out = to_str_u32(cfg.rssi.interval_ms.load()); return true; }
    }
    if (mon == "tcp_loss") {
        if (field == "interval" || field == "interval_ms") { *value_out = to_str_u32(cfg.tcp_loss.interval_ms.load()); return true; }
    }
    if (mon == "traffic") {
        if (field == "interval" || field == "interval_ms") { *value_out = to_str_u32(cfg.traffic.interval_ms.load()); return true; }
    }
    if (mon == "quality") {
        if (field == "interval" || field == "interval_ms") { *value_out = to_str_u32(cfg.quality.interval_ms.load()); return true; }
    }
    if (mon == "bluetooth") {
        if (field == "interval" || field == "interval_ms") { *value_out = to_str_u32(cfg.bluetooth.interval_ms.load()); return true; }
    }
    if (mon == "dns") {
        if (field == "interval" || field == "interval_ms") { *value_out = to_str_u32(cfg.dns.interval_ms.load()); return true; }
        if (field == "capture_pages") { *value_out = to_str_u32(cfg.dns.capture_pages.load()); return true; }
    }
    if (mon == "tcp_connect") {
        if (field == "interval" || field == "interval_ms") { *value_out = to_str_u32(cfg.tcp_connect.interval_ms.load()); return true; }
        if (field == "capture_pages") { *value_out = to_str_u32(cfg.tcp_connect.capture_pages.load()); return true; }
    }
    if (mon == "wifi_loss") {
        if (field == "interval" || field == "interval_ms") { *value_out = to_str_u32(cfg.wifi_loss.interval_ms.load()); return true; }
    }
    if (mon == "http_latency") {
        if (field == "interval" || field == "interval_ms") { *value_out = to_str_u32(cfg.http_latency.interval_ms.load()); return true; }
    }
    if (mon == "process_profiler") {
        if (field == "interval" || field == "interval_ms") { *value_out = to_str_u32(cfg.process_profiler.interval_ms.load()); return true; }
    }
    if (mon == "tcp_retrans") {
        if (field == "interval" || field == "interval_ms") { *value_out = to_str_u32(cfg.tcp_retrans.interval_ms.load()); return true; }
    }
    if (mon == "tcp_conn") {
        if (field == "interval" || field == "interval_ms") { *value_out = to_str_u32(cfg.tcp_conn.interval_ms.load()); return true; }
    }
    if (mon == "skb_drop") {
        if (field == "interval" || field == "interval_ms") { *value_out = to_str_u32(cfg.skb_drop.interval_ms.load()); return true; }
    }
    if (mon == "edge") {
        if (field == "interval" || field == "interval_ms") { *value_out = to_str_u32(cfg.edge.interval_ms.load()); return true; }
        if (field == "timeout" || field == "timeout_ms") { *value_out = to_str_u32(cfg.edge.timeout_ms.load()); return true; }
    }
    return false;
}

}  // namespace

bool isTrialableKey(const std::string& key) {
    return isTrialableKeyImpl(key);
}

bool snapshotMonitorParam(const WeakNetConfig& cfg, const std::string& key,
                          std::string* value_out) {
    return snapshotMonitorParamImpl(cfg, key, value_out);
}

ConfigTransaction::ConfigTransaction(std::string state_path)
    : state_path_(std::move(state_path)) {}

ConfigState ConfigTransaction::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

TrialWindow ConfigTransaction::trial() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return trial_;
}

bool ConfigTransaction::startTrial(const WeakNetConfig& cfg, const std::string& key,
                                    const std::string& new_value,
                                    const std::string& action_id,
                                    uint64_t generation, std::string* error) {
    if (!isTrialableKeyImpl(key)) {
        if (error) *error = "key not trialable: " + key;
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == ConfigState::TRIAL) {
        if (error) *error = "trial_in_progress";
        return false;
    }
    if (generation <= last_applied_generation_.load()) {
        if (error) *error = "stale_generation";
        return false;
    }

    std::string prior;
    if (!snapshotMonitorParamImpl(cfg, key, &prior)) {
        if (error) *error = "cannot snapshot prior value for key: " + key;
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    trial_.armed_at = now;
    trial_.deadline = now + window_;
    trial_.generation = generation;
    trial_.pending_action_id = action_id;
    trial_.pending_keys.clear();
    trial_.pending_keys[key] = new_value;
    trial_.prior_values.clear();
    trial_.prior_values[key] = prior;
    state_ = ConfigState::TRIAL;

    persistPriorValues();
    return true;
}

bool ConfigTransaction::extendTrial(const WeakNetConfig& cfg, const std::string& key,
                                     const std::string& new_value,
                                     const std::string& action_id,
                                     uint64_t generation, std::string* error) {
    if (!isTrialableKeyImpl(key)) {
        if (error) *error = "key not trialable: " + key;
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != ConfigState::TRIAL) {
        if (error) *error = "not_in_trial";
        return false;
    }
    if (generation <= trial_.generation) {
        if (error) *error = "stale_generation";
        return false;
    }

    // 只在该 key 第一次被叠入时记录 prior；覆盖时保留初值。
    if (trial_.prior_values.find(key) == trial_.prior_values.end()) {
        std::string prior;
        if (!snapshotMonitorParamImpl(cfg, key, &prior)) {
            if (error) *error = "cannot snapshot prior value for key: " + key;
            return false;
        }
        trial_.prior_values[key] = prior;
    }
    trial_.pending_keys[key] = new_value;
    trial_.pending_action_id = action_id;
    trial_.generation = generation;
    trial_.deadline = std::chrono::steady_clock::now() + window_;

    persistPriorValues();
    return true;
}

bool ConfigTransaction::confirmStable(uint64_t generation, std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != ConfigState::TRIAL) {
        if (error) *error = "not_in_trial";
        return false;
    }
    if (generation != trial_.generation) {
        if (error) *error = "generation_mismatch";
        return false;
    }
    markGenerationApplied(generation);
    trial_ = TrialWindow{};
    state_ = ConfigState::STABLE;
    clearPersistedState();
    return true;
}

bool ConfigTransaction::forceRollback(WeakNetConfig* cfg, const std::string& reason) {
    if (!cfg) return false;
    TrialWindow snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != ConfigState::TRIAL) {
            // 支持崩溃恢复路径：若当前不是 TRIAL，尝试从持久化磁盘文件加载 prior_values
            std::map<std::string, std::string> disk_priors;
            if (loadPriorValues(&disk_priors)) {
                snapshot.prior_values = std::move(disk_priors);
            } else {
                return false;
            }
        } else {
            snapshot = trial_;
        }
    }

    // 还原时按 prior_values 应用：把每个 key 设回 prior 值。
    // 失败也要继续——半还原比不还原强，剩余错误进日志。
    bool all_ok = true;
    for (const auto& [key, prior] : snapshot.prior_values) {
        std::string err;
        if (!setMonitorParam(cfg, key, prior, &err)) {
            all_ok = false;
            // 日志由调用方写；这里只汇总。
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        // ROLLBACK 是瞬态：restore 完成立即回 STABLE。
        if (snapshot.generation > 0) {
            markGenerationApplied(snapshot.generation);
        }
        trial_ = TrialWindow{};
        state_ = ConfigState::STABLE;
    }
    clearPersistedState();
    (void)reason;  // 由调用方写日志/回执
    return all_ok;
}

bool ConfigTransaction::deadlineExpired(std::chrono::steady_clock::time_point now) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == ConfigState::TRIAL && now >= trial_.deadline;
}

bool ConfigTransaction::hasCrashRecoveryFile() const {
    std::ifstream probe(state_path_);
    return probe.is_open();
}

void ConfigTransaction::clearPersistedState() {
    if (state_path_.empty()) return;
    std::remove(state_path_.c_str());
}

bool ConfigTransaction::persistPriorValues() const {
    if (state_path_.empty()) return false;
    const std::string tmp = state_path_ + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out.is_open()) return false;
        for (const auto& [k, v] : trial_.prior_values) {
            out << k << kTxnStateFieldSep << v << '\n';
        }
        out.flush();
        if (!out.good()) return false;
    }
    return std::rename(tmp.c_str(), state_path_.c_str()) == 0;
}

bool ConfigTransaction::loadPriorValues(std::map<std::string, std::string>* out) const {
    if (!out) return false;
    std::ifstream in(state_path_);
    if (!in.is_open()) return false;
    out->clear();
    std::string line;
    while (std::getline(in, line)) {
        const auto tab = line.find(kTxnStateFieldSep);
        if (tab == std::string::npos) continue;
        (*out)[line.substr(0, tab)] = line.substr(tab + 1);
    }
    return !out->empty();
}

std::string serializeMonitorJson(const WeakNetConfig& cfg, const std::string& monitor,
                                 std::string* error) {
    // 未知 monitor 直接报错（支持 "all" + 13 个监控器 + "server" + "edge"）
    static const std::set<std::string> valid = {
        "all", "server", "rtt", "jitter", "rssi", "tcp_loss", "traffic", "quality",
        "bluetooth", "dns", "wifi_loss", "http_latency", "process_profiler",
        "tcp_retrans", "tcp_conn", "skb_drop", "tcp_connect", "active_probe", "edge"
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
        writeUint("capture_pages", cfg.dns.capture_pages.load());
        writeString("assessment_profile", cfg.dns.assessment_profile.get());
        json.seekp(-1, std::ios_base::cur); json << "},";
    }
    if (monitor == "all" || monitor == "active_probe") {
        json << "\"active_probe\":{";
        writeBool("enabled", cfg.active_probe.enabled.load());
        writeUint("interval_ms", cfg.active_probe.interval_ms.load());
        writeUint("timeout_ms", cfg.active_probe.timeout_ms.load());
        writeString("targets", cfg.active_probe.targets.get());
        writeBool("https_enabled", cfg.active_probe.https_enabled.load());
        writeBool("portal_check_enabled", cfg.active_probe.portal_check_enabled.load());
        writeString("portal_targets", cfg.active_probe.portal_targets.get());
        writeString("portal_path", cfg.active_probe.portal_path.get());
        writeString("portal_expect_body", cfg.active_probe.portal_expect_body.get());
        json.seekp(-1, std::ios_base::cur); json << "},";
    }
    if (monitor == "all" || monitor == "tcp_connect") {
        json << "\"tcp_connect\":{";
        writeBool("enabled", cfg.tcp_connect.enabled.load());
        writeString("bpf_obj", cfg.tcp_connect.bpf_obj.get());
        writeUint("interval_ms", cfg.tcp_connect.interval_ms.load());
        writeUint("capture_pages", cfg.tcp_connect.capture_pages.load());
        json.seekp(-1, std::ios_base::cur); json << "},";
    }
    if (monitor == "all" || monitor == "edge") {
        json << "\"edge\":{";
        writeBool("enabled", cfg.edge.enabled.load());
        writeUint("interval_ms", cfg.edge.interval_ms.load());
        writeUint("timeout_ms", cfg.edge.timeout_ms.load());
        writeString("url", cfg.edge.url.get());
        writeString("tenant", cfg.edge.tenant.get());
        writeString("device_id", cfg.edge.device_id.get());
        writeString("key_id", cfg.edge.key_id.get());
        // 刻意**不**回显 token 与私钥路径：
        //   - token 是凭据，序列化出去就等于把它散播到日志/前端；
        //   - 私钥路径会暴露主机的凭据布局，且对排障无增益。
        // 需要确认"是否已配置"时，看 enabled + url + device_id 即可。
        writeBool("token_configured", !cfg.edge.token.get().empty());
        writeBool("private_key_configured", !cfg.edge.private_key_path.get().empty());
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
    if (monitor == "all" || monitor == "skb_drop") {
        json << "\"skb_drop\":{";
        writeBool("enabled", cfg.skb_drop.enabled.load());
        writeString("bpf_obj", cfg.skb_drop.bpf_obj.get());
        writeUint("interval_ms", cfg.skb_drop.interval_ms.load());
        json.seekp(-1, std::ios_base::cur); json << "},";
    }

    // 去掉最后一个逗号
    auto s = json.str();
    if (!s.empty() && s.back() == ',') s.pop_back();
    s.push_back('}');
    return s;
}

}  // namespace weaknet_dbus
