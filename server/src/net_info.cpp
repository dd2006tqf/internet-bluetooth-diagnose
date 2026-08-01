// net_info.cpp
// NetInfo 数据验证与序列化/反序列化实现

#include "net_info.hpp"
#include "serializer.hpp"
#include "logger.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <type_traits>

namespace weaknet_dbus {

namespace {

// 二进制序列化格式版本号，写入缓冲区头部，用于后续向后兼容
constexpr int32_t kBinaryFormatVersion = 1;

// 将任意 POD 类型按字节追加到缓冲区
template <typename T>
void appendBytes(const T& value, std::vector<uint8_t>& out) {
    static_assert(std::is_trivially_copyable<T>::value, "T must be trivially copyable");
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&value);
    out.insert(out.end(), p, p + sizeof(T));
}

// 从缓冲区 offset 处读取一个 POD 类型，越界返回 false
template <typename T>
bool readBytes(const std::vector<uint8_t>& buffer, size_t& offset, T& out) {
    static_assert(std::is_trivially_copyable<T>::value, "T must be trivially copyable");
    if (offset + sizeof(T) > buffer.size()) return false;
    std::memcpy(&out, buffer.data() + offset, sizeof(T));
    offset += sizeof(T);
    return true;
}

// ---------------------------------------------------------------------------
// JSON 工具函数
// ---------------------------------------------------------------------------

// 对字符串进行 JSON 转义，避免接口名/等级文本中含有特殊字符破坏 JSON 结构
std::string escapeJsonString(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                // 控制字符（< 0x20）使用 \uXXXX 形式转义
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
                break;
        }
    }
    return out;
}

// 跳过 JSON 中的空白字符，返回首个非空白字符位置，越界返回 npos
size_t skipWhitespace(const std::string& s, size_t pos) {
    while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) {
        ++pos;
    }
    return pos;
}

// 在 pos 处解析一个 JSON 字符串（含两侧引号），成功返回 true 并推进 pos
// 支持常见转义序列；不支持的转义按原字符处理
bool parseJsonString(const std::string& s, size_t& pos, std::string& out) {
    pos = skipWhitespace(s, pos);
    if (pos >= s.size() || s[pos] != '"') return false;
    ++pos;  // 跳过起始引号
    out.clear();
    while (pos < s.size()) {
        char c = s[pos++];
        if (c == '"') {
            return true;  // 字符串结束
        }
        if (c == '\\' && pos < s.size()) {
            char esc = s[pos++];
            switch (esc) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'u': {
                    // \uXXXX：取后续 4 位十六进制，仅处理基本平面的常见字符
                    if (pos + 4 > s.size()) return false;
                    char buf[5] = {s[pos], s[pos + 1], s[pos + 2], s[pos + 3], '\0'};
                    char* end = nullptr;
                    unsigned long code = std::strtoul(buf, &end, 16);
                    if (end != buf + 4) return false;
                    pos += 4;
                    if (code < 0x80) {
                        out += static_cast<char>(code);
                    } else if (code < 0x800) {
                        out += static_cast<char>(0xC0 | (code >> 6));
                        out += static_cast<char>(0x80 | (code & 0x3F));
                    } else {
                        out += static_cast<char>(0xE0 | (code >> 12));
                        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (code & 0x3F));
                    }
                    break;
                }
                default:
                    // 未知转义：保留原字符，保证向前兼容
                    out += esc;
                    break;
            }
        } else {
            out += c;
        }
    }
    return false;  // 未遇到结束引号
}

// 在 pos 处解析一个 JSON 值（字符串/数字/布尔），以值结束后的位置推进 pos
// 数字/布尔通过字符串形式返回，由调用方按需转换
bool parseJsonValue(const std::string& s, size_t& pos, std::string& out) {
    pos = skipWhitespace(s, pos);
    if (pos >= s.size()) return false;
    if (s[pos] == '"') {
        return parseJsonString(s, pos, out);
    }
    // 数字或 true/false：读取直到遇到逗号、} 或空白
    out.clear();
    while (pos < s.size()) {
        char c = s[pos];
        if (c == ',' || c == '}' || c == ']' || std::isspace(static_cast<unsigned char>(c))) {
            break;
        }
        out += c;
        ++pos;
    }
    return !out.empty();
}

// 安全地将字符串转换为 int，失败返回 fallback
int safeStoi(const std::string& s, int fallback) {
    errno = 0;
    char* end = nullptr;
    long v = std::strtol(s.c_str(), &end, 10);
    if (errno != 0 || end == s.c_str() || *end != '\0') return fallback;
    if (v < INT32_MIN || v > INT32_MAX) return fallback;
    return static_cast<int>(v);
}

// 安全地将字符串转换为 double，失败返回 fallback
double safeStod(const std::string& s, double fallback) {
    errno = 0;
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    if (errno != 0 || end == s.c_str() || *end != '\0') return fallback;
    return v;
}

// 安全地将字符串转换为 uint64，失败返回 fallback
// 注意：strtoull 对负数输入会回绕为巨大的无符号值，需显式拒绝
uint64_t safeStou64(const std::string& s, uint64_t fallback) {
    // 检查首个非空白字符，拒绝负数
    size_t start = s.find_first_not_of(" \t");
    if (start == std::string::npos) return fallback;
    if (s[start] == '-') return fallback;
    errno = 0;
    char* end = nullptr;
    unsigned long long v = std::strtoull(s.c_str(), &end, 10);
    if (errno != 0 || end == s.c_str() || *end != '\0') return fallback;
    return static_cast<uint64_t>(v);
}

}  // namespace

// ===========================================================================
// 数据验证
// ===========================================================================

bool NetInfo::isValid() const {
    // 接口名不能为空
    if (ifname_.empty()) return false;

    // RTT：-1（未测量）或非负值
    if (rtt_ms_ < -1) return false;

    // 上一次 RTT：-1（未测量）或非负值
    if (prev_rtt_ms_ < -1) return false;

    // 丢包率：-1（未测量）或 [0, 100]
    if (tcp_loss_rate_ < -1.0 || tcp_loss_rate_ > 100.0) return false;

    // RSSI：-1000（未测量/非 Wi-Fi）或 [-100, 0]
    if (rssi_dbm_ != -1000 && (rssi_dbm_ < -100 || rssi_dbm_ > 0)) return false;

    // 抖动：-1（未测量）或非负值
    if (jitter_ms_ < -1.0) return false;

    // 蓝牙距离：-1.0（未测量）或非负值
    if (bt_distance_ < -1.0) return false;

    // 蓝牙音频质量：可为空字符串，非空时须为合法枚举值
    if (!bt_audio_quality_.empty()) {
        static const std::string validLevels[] = {"excellent", "good", "fair", "poor", "unknown"};
        bool found = false;
        for (const auto& l : validLevels) {
            if (bt_audio_quality_ == l) { found = true; break; }
        }
        if (!found) return false;
    }

    // 频段冲突置信度：须在 [0, 100] 区间内
    if (band_conflict_confidence_ < 0.0 || band_conflict_confidence_ > 100.0) return false;

    return true;
}

bool NetInfo::needsUpdate(const NetInfo& other) const {
    // 接口名不同则不属于同一对象，无需比较
    if (!sameKey(other)) return true;

    // equals 已覆盖 ifname/is_default/type/rtt/state 五个关键字段
    if (!equals(other)) return true;

    // 进一步比较动态采集指标
    return rssi_dbm_ != other.rssi_dbm_
        || tcp_loss_rate_ != other.tcp_loss_rate_
        || traffic_total_bps_ != other.traffic_total_bps_
        || traffic_total_pps_ != other.traffic_total_pps_
        || traffic_active_flows_ != other.traffic_active_flows_
        || jitter_ms_ != other.jitter_ms_
        || quality_ != other.quality_
        || bt_distance_ != other.bt_distance_
        || bt_audio_quality_ != other.bt_audio_quality_
        || band_conflict_ != other.band_conflict_
        || band_conflict_confidence_ != other.band_conflict_confidence_;
}

// ===========================================================================
// JSON 序列化/反序列化
// ===========================================================================

std::string NetInfo::toJson() const {
    std::ostringstream json;
    json << "{";
    json << "\"ifname\":\"" << escapeJsonString(ifname_) << "\",";
    json << "\"is_default\":" << (is_default_ ? "true" : "false") << ",";
    json << "\"type\":" << static_cast<int>(type_) << ",";
    json << "\"state\":" << static_cast<int>(state_) << ",";
    json << "\"using_now\":" << (using_now_ ? "true" : "false") << ",";
    json << "\"quality\":" << static_cast<int>(quality_) << ",";
    json << "\"rtt_ms\":" << rtt_ms_ << ",";
    json << "\"prev_rtt_ms\":" << prev_rtt_ms_ << ",";
    json << "\"rssi_dbm\":" << rssi_dbm_ << ",";
    json << "\"tcp_loss_rate\":" << std::fixed << std::setprecision(2) << tcp_loss_rate_ << ",";
    json << "\"tcp_loss_level\":\"" << escapeJsonString(tcp_loss_level_) << "\",";
    json << "\"traffic_bps\":" << traffic_total_bps_ << ",";
    json << "\"traffic_pps\":" << traffic_total_pps_ << ",";
    json << "\"active_flows\":" << traffic_active_flows_ << ",";
    json << "\"jitter_ms\":" << std::fixed << std::setprecision(1) << jitter_ms_ << ",";
    json << "\"jitter_level\":\"" << escapeJsonString(jitter_level_) << "\",";
    json << "\"band_conflict\":" << (band_conflict_ ? "true" : "false") << ",";
    json << "\"band_conflict_confidence\":" << std::fixed << std::setprecision(1) << band_conflict_confidence_;
    json << "}";
    return json.str();
}

bool NetInfo::fromJson(const std::string& json) {
    if (json.empty()) {
        LOG_ERROR(LogModule::WEAK_MGR, "fromJson: empty JSON input");
        return false;
    }

    // 使用临时对象构建，仅当全部解析成功时才提交到当前对象，保证失败不变性
    NetInfo tmp;

    size_t pos = skipWhitespace(json, 0);
    if (pos >= json.size() || json[pos] != '{') return false;
    ++pos;

    bool first = true;
    while (pos < json.size()) {
        pos = skipWhitespace(json, pos);
        if (pos >= json.size()) return false;

        // 对象结束
        if (json[pos] == '}') {
            ++pos;
            *this = std::move(tmp);
            return true;
        }

        // 非首个字段需要先消费逗号
        if (!first) {
            if (json[pos] != ',') return false;
            ++pos;
            pos = skipWhitespace(json, pos);
        }
        first = false;

        // 解析 key
        std::string key;
        if (!parseJsonString(json, pos, key)) return false;

        // 消费冒号
        pos = skipWhitespace(json, pos);
        if (pos >= json.size() || json[pos] != ':') return false;
        ++pos;

        // 解析 value
        std::string value;
        if (!parseJsonValue(json, pos, value)) return false;

        // 按 key 分发到对应字段
        if (key == "ifname") {
            tmp.ifname_ = value;
        } else if (key == "is_default") {
            tmp.is_default_ = (value == "true");
        } else if (key == "type") {
            int v = safeStoi(value, 0);
            if (v < 0 || v > static_cast<int>(NetType::Cellular)) return false;
            tmp.type_ = static_cast<NetType>(v);
        } else if (key == "state") {
            int v = safeStoi(value, 0);
            if (v < 0 || v > static_cast<int>(NetState::Up)) return false;
            tmp.state_ = static_cast<NetState>(v);
        } else if (key == "using_now") {
            tmp.using_now_ = (value == "true");
        } else if (key == "quality") {
            int v = safeStoi(value, 0);
            if (v < 0 || v > static_cast<int>(LinkQuality::Bad)) return false;
            tmp.quality_ = static_cast<LinkQuality>(v);
        } else if (key == "rtt_ms") {
            tmp.rtt_ms_ = safeStoi(value, -1);
        } else if (key == "prev_rtt_ms") {
            tmp.prev_rtt_ms_ = safeStoi(value, -1);
        } else if (key == "rssi_dbm") {
            tmp.rssi_dbm_ = safeStoi(value, -1000);
        } else if (key == "tcp_loss_rate") {
            tmp.tcp_loss_rate_ = safeStod(value, -1.0);
        } else if (key == "tcp_loss_level") {
            tmp.tcp_loss_level_ = value;
        } else if (key == "traffic_bps") {
            tmp.traffic_total_bps_ = safeStou64(value, 0);
        } else if (key == "traffic_pps") {
            tmp.traffic_total_pps_ = safeStou64(value, 0);
        } else if (key == "active_flows") {
            int v = safeStoi(value, 0);
            tmp.traffic_active_flows_ = (v < 0) ? 0 : static_cast<uint32_t>(v);
        } else if (key == "jitter_ms") {
            tmp.jitter_ms_ = safeStod(value, -1.0);
        } else if (key == "jitter_level") {
            tmp.jitter_level_ = value;
        } else if (key == "bt_distance") {
            tmp.bt_distance_ = safeStod(value, -1.0);
        } else if (key == "bt_audio_quality") {
            tmp.bt_audio_quality_ = value;
        } else if (key == "band_conflict") {
            tmp.band_conflict_ = (value == "true");
        } else if (key == "band_conflict_confidence") {
            tmp.band_conflict_confidence_ = safeStod(value, 0.0);
        }
        // 未知字段：忽略，保证向前兼容
    }
    return false;  // 未遇到闭合的 '}'
}

// ===========================================================================
// 二进制序列化/反序列化
// ===========================================================================

std::vector<uint8_t> NetInfo::toBinary() const {
    std::vector<uint8_t> buf;
    // 版本号头，便于后续格式演进时做兼容判断
    serializeInt32(kBinaryFormatVersion, buf);
    serializeString(ifname_, buf);
    serializeInt32(is_default_ ? 1 : 0, buf);
    serializeInt32(static_cast<int32_t>(type_), buf);
    serializeInt32(static_cast<int32_t>(state_), buf);
    serializeInt32(using_now_ ? 1 : 0, buf);
    serializeInt32(static_cast<int32_t>(quality_), buf);
    serializeInt32(rtt_ms_, buf);
    serializeInt32(prev_rtt_ms_, buf);
    serializeInt32(rssi_dbm_, buf);
    // tcp_loss_rate 与 jitter_ms 以 IEEE-754 双精度存储，避免精度丢失
    appendBytes(tcp_loss_rate_, buf);
    appendBytes(jitter_ms_, buf);
    serializeString(tcp_loss_level_, buf);
    serializeString(jitter_level_, buf);
    // 流量统计
    appendBytes(traffic_total_bps_, buf);
    appendBytes(traffic_total_pps_, buf);
    appendBytes(traffic_active_flows_, buf);
    // 蓝牙相关扩展字段
    appendBytes(bt_distance_, buf);
    serializeString(bt_audio_quality_, buf);
    serializeInt32(band_conflict_ ? 1 : 0, buf);
    appendBytes(band_conflict_confidence_, buf);
    return buf;
}

bool NetInfo::fromBinary(const std::vector<uint8_t>& buffer) {
    if (buffer.empty()) {
        LOG_ERROR(LogModule::WEAK_MGR, "fromBinary: empty buffer");
        return false;
    }

    NetInfo tmp;
    size_t offset = 0;

    // 版本号校验
    int32_t version = 0;
    if (!deserializeInt32(buffer, offset, version)) return false;
    if (version != kBinaryFormatVersion) return false;

    // 顺序与 toBinary 严格一致
    if (!deserializeString(buffer, offset, tmp.ifname_)) return false;

    int32_t is_default = 0;
    if (!deserializeInt32(buffer, offset, is_default)) return false;
    tmp.is_default_ = (is_default != 0);

    int32_t type_val = 0;
    if (!deserializeInt32(buffer, offset, type_val)) return false;
    if (type_val < 0 || type_val > static_cast<int32_t>(NetType::Cellular)) return false;
    tmp.type_ = static_cast<NetType>(type_val);

    int32_t state_val = 0;
    if (!deserializeInt32(buffer, offset, state_val)) return false;
    if (state_val < 0 || state_val > static_cast<int32_t>(NetState::Up)) return false;
    tmp.state_ = static_cast<NetState>(state_val);

    int32_t using_now = 0;
    if (!deserializeInt32(buffer, offset, using_now)) return false;
    tmp.using_now_ = (using_now != 0);

    int32_t quality_val = 0;
    if (!deserializeInt32(buffer, offset, quality_val)) return false;
    if (quality_val < 0 || quality_val > static_cast<int32_t>(LinkQuality::Bad)) return false;
    tmp.quality_ = static_cast<LinkQuality>(quality_val);

    if (!deserializeInt32(buffer, offset, tmp.rtt_ms_)) return false;
    if (!deserializeInt32(buffer, offset, tmp.prev_rtt_ms_)) return false;
    if (!deserializeInt32(buffer, offset, tmp.rssi_dbm_)) return false;

    // 双精度浮点字段
    if (!readBytes(buffer, offset, tmp.tcp_loss_rate_)) return false;
    if (!readBytes(buffer, offset, tmp.jitter_ms_)) return false;

    if (!deserializeString(buffer, offset, tmp.tcp_loss_level_)) return false;
    if (!deserializeString(buffer, offset, tmp.jitter_level_)) return false;

    // 流量统计
    if (!readBytes(buffer, offset, tmp.traffic_total_bps_)) return false;
    if (!readBytes(buffer, offset, tmp.traffic_total_pps_)) return false;
    if (!readBytes(buffer, offset, tmp.traffic_active_flows_)) return false;

    // 蓝牙相关扩展字段
    if (!readBytes(buffer, offset, tmp.bt_distance_)) return false;
    if (!deserializeString(buffer, offset, tmp.bt_audio_quality_)) return false;
    int32_t band_conflict_val = 0;
    if (!deserializeInt32(buffer, offset, band_conflict_val)) return false;
    tmp.band_conflict_ = (band_conflict_val != 0);
    if (!readBytes(buffer, offset, tmp.band_conflict_confidence_)) return false;

    *this = std::move(tmp);
    return true;
}

}  // namespace weaknet_dbus
