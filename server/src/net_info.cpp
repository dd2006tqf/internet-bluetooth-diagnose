/**
 * @file net_info.cpp
 * @brief NetInfo 数据类的验证与 JSON 序列化/反序列化实现
 *
 * @details 本文件实现 NetInfo（描述单个网络接口健康状况的核心数据类）的以下能力：
 *
 *          1. **数据验证**（isValid）：检查各字段的合法值范围，确保数据完整性和一致性。
 *             哨兵值约定（表示"未测量"）：
 *             - rtt_ms_ / prev_rtt_ms_: -1
 *             - tcp_loss_rate_ / jitter_ms_ / bt_distance_: -1.0
 *             - rssi_dbm_: -1000（同时区分非 Wi-Fi 接口）
 *             - bt_audio_quality_: 空字符串表示未测量
 *
 *          2. **JSON 序列化/反序列化**（toJson / fromJson）：
 *             使用自定义轻量 JSON 解析器（不依赖第三方库如 nlohmann/json），
 *             支持转义序列、未知字段忽略（向前兼容）、失败不变性（临时对象模式）。
 *
 * @note 本文件不依赖系统调用或 netlink/ioctl/wpa_supplicant，
 *       纯数据层实现，与网络采集层解耦。
 */

#include "net_info.hpp"
#include "logger.hpp"
#include "utils/json_escape.hpp"

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

// ---------------------------------------------------------------------------
// 轻量 JSON 工具函数（纯字符串操作，无需外部依赖）
// ---------------------------------------------------------------------------

/**
 * @brief 在 JSON 字符串中跳过连续空白字符
 * @param s   完整 JSON 字符串
 * @param pos 当前位置
 * @return 首个非空白字符的位置；全为空白则返回 s.size()
 */
size_t skipWhitespace(const std::string& s, size_t pos) {
    while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) {
        ++pos;
    }
    return pos;
}

/**
 * @brief 在 pos 处解析一个 JSON 字符串（含两侧引号）
 *
 * 完整支持的转义序列：\", \\, \/, \b, \f, \n, \r, \t, \uXXXX
 * \uXXXX 仅处理基本平面字符：编码值 0x00-0x7F 直接返回，
 * 0x80-0x7FF 输出 UTF-8 两字节序列，0x800-0xFFFF 输出三字节序列。
 *
 * @param s   JSON 源字符串
 * @param pos [in,out] 当前位置，成功后推进到结束引号之后
 * @param out [out] 去除引号和解码转义后的字符串内容
 *
 * @return true  - 完整解析成功
 *         false - 遇到非起始引号、转义序列不完整、或未匹配结束引号
 */
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

/**
 * @brief 在 pos 处解析一个 JSON 值（字符串 / 数字 / 布尔）
 *
 * 字符串值委托给 parseJsonString；
 * 数字/布尔值通过扫描直到遇到终止字符（逗号/}/]/空白）来提取原始 token。
 * 原始 token 由调用方按需安全转换为目标类型（safeStoi / safeStod / safeStou64）。
 *
 * @param s   JSON 源字符串
 * @param pos [in,out] 当前位置
 * @param out [out] 值的原始字符串形式
 *
 * @return true  - 至少提取到一个字符
 */
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

/** @brief 安全字符串转 int（strtol 包装），失败返回 fallback */
int safeStoi(const std::string& s, int fallback) {
    errno = 0;
    char* end = nullptr;
    long v = std::strtol(s.c_str(), &end, 10);
    if (errno != 0 || end == s.c_str() || *end != '\0') return fallback;
    if (v < INT32_MIN || v > INT32_MAX) return fallback;
    return static_cast<int>(v);
}

/** @brief 安全字符串转 double（strtod 包装），失败返回 fallback */
double safeStod(const std::string& s, double fallback) {
    errno = 0;
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    if (errno != 0 || end == s.c_str() || *end != '\0') return fallback;
    return v;
}

/**
 * @brief 安全字符串转 uint64（strtoull 包装）
 *
 * 显式拒绝负数输入（strtoull 对负数会回绕为巨大无符号值，必须前置检查）。
 *
 * @param s        输入字符串
 * @param fallback 失败时返回的值
 */
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

/**
 * @brief 检查 NetInfo 各字段是否在合法范围内
 *
 * 校验规则：
 * - ifname_：不能为空
 * - rtt_ms_ / prev_rtt_ms_：-1（未测量）或 ≥ 0
 * - tcp_loss_rate_：[-1, 100]（-1 未测量）
 * - rssi_dbm_：-1000（未测量/非 Wi-Fi）或 [-100, 0]（dBm 范围）
 * - jitter_ms_：[-1, +∞)
 * - bt_distance_：[-1, +∞)
 * - bt_audio_quality_：可为空；非空时值必须在 {excellent, good, fair, poor, unknown} 中
 * - band_conflict_confidence_：[0, 100]
 *
 * @return true  - 所有字段合法
 */
bool NetInfo::isValid() const {
    // 接口名不能为空
    if (ifname_.empty()) return false;

    // RTT：-1（未测量）或非负值
    if (rtt_ms_ < -1) return false;

    // 上一次 RTT：-1（未测量）或非负值
    if (prev_rtt_ms_ < -1) return false;

    // 丢包率：-1（未测量）或 [0, 100]
    if (tcp_loss_rate_ < -1.0 || tcp_loss_rate_ > 100.0) return false;

    // RSSI：-1000（未测量/非 Wi-Fi）或物理合理 dBm [-100, 0]
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

// ===========================================================================
// JSON 序列化/反序列化
// ===========================================================================

/**
 * @brief 将 NetInfo 序列化为 JSON 字符串
 *
 * 使用 std::ostringstream 拼接，字符串字段通过 weaknet_utils::escapeJsonString 转义。
 * 浮点字段通过 std::fixed + std::setprecision 控制小数位数，
 * 保证输出精度稳定且不出现科学计数法。
 *
 * @return 完整 JSON 对象字符串，如 {"ifname":"eth0","rtt_ms":10,...}
 */
std::string NetInfo::toJson() const {
    std::ostringstream json;
    json << "{";
    json << "\"ifname\":\"" << weaknet_utils::escapeJsonString(ifname_) << "\",";
    json << "\"is_default\":" << (is_default_ ? "true" : "false") << ",";
    json << "\"type\":" << static_cast<int>(type_) << ",";
    json << "\"state\":" << static_cast<int>(state_) << ",";
    json << "\"using_now\":" << (using_now_ ? "true" : "false") << ",";
    json << "\"quality\":" << static_cast<int>(quality_) << ",";
    json << "\"rtt_ms\":" << rtt_ms_ << ",";
    json << "\"prev_rtt_ms\":" << prev_rtt_ms_ << ",";
    json << "\"rssi_dbm\":" << rssi_dbm_ << ",";
    json << "\"rssi_estimated\":" << (rssi_estimated_ ? "true" : "false") << ",";
    json << "\"rssi_source\":\"" << weaknet_utils::escapeJsonString(rssi_source_) << "\",";
    json << "\"rtt_sample_ts\":" << rtt_sample_ts_ms_ << ",";
    json << "\"rssi_sample_ts\":" << rssi_sample_ts_ms_ << ",";
    json << "\"jitter_sample_ts\":" << jitter_sample_ts_ms_ << ",";
    json << "\"tcp_loss_sample_ts\":" << tcp_loss_sample_ts_ms_ << ",";
    json << "\"traffic_sample_ts\":" << traffic_sample_ts_ms_ << ",";
    json << "\"tcp_loss_rate\":" << std::fixed << std::setprecision(2) << tcp_loss_rate_ << ",";
    json << "\"tcp_loss_level\":\"" << weaknet_utils::escapeJsonString(tcp_loss_level_) << "\",";
    json << "\"traffic_bps\":" << traffic_total_bps_ << ",";
    json << "\"traffic_pps\":" << traffic_total_pps_ << ",";
    json << "\"active_flows\":" << traffic_active_flows_ << ",";
    json << "\"jitter_ms\":" << std::fixed << std::setprecision(1) << jitter_ms_ << ",";
    json << "\"jitter_level\":\"" << weaknet_utils::escapeJsonString(jitter_level_) << "\",";
    json << "\"band_conflict\":" << (band_conflict_ ? "true" : "false") << ",";
    json << "\"band_conflict_confidence\":" << std::fixed << std::setprecision(1) << band_conflict_confidence_;
    json << "}";
    return json.str();
}

/**
 * @brief 从 JSON 字符串反序列化到 NetInfo
 *
 * 采用"临时对象 + 最后提交"模式：
 * 1. 创建 NetInfo tmp；
 * 2. 逐字段解析填充 tmp，期间任何失败直接 return false，
 *    this 对象保持原值不变（失败不变性）；
 * 3. 完整解析后 *this = std::move(tmp)。
 *
 * 关键设计：
 * - 未知字段被忽略（向前兼容：新增字段不影响旧版本解析）
 * - 枚举类型（type/state/quality）带范围校验
 * - 使用 safeStoi/safeStod/safeStou64 避免数字解析异常
 *
 * @param json 完整 JSON 对象字符串
 *
 * @return true  - 完整解析成功并提交到 this
 *         false - 解析失败（this 保持原值）
 */
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
        } else if (key == "rssi_estimated") {
            tmp.rssi_estimated_ = (value == "true");
        } else if (key == "rssi_source") {
            tmp.rssi_source_ = value;
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

}  // namespace weaknet_dbus
