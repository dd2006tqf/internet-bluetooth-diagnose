/**
 * @file net_info.cpp
 * @brief NetInfo 数据类的验证、JSON/二进制序列化与反序列化实现
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
 *          3. **二进制序列化/反序列化**（toBinary / fromBinary）：
 *             自定义紧凑二进制格式：32 位小端序头 + POD 字段字节拼接 + 字符串
 *             length-prefixed 编码。格式版本号在缓冲区头部，便于未来格式演进。
 *             双精度浮点字段（tcp_loss_rate_、jitter_ms_、band_conflict_confidence_）
 *             以 IEEE-754 原样存储避免精度丢失。
 *
 * @note 本文件不依赖系统调用或 netlink/ioctl/wpa_supplicant，
 *       纯数据层实现，与网络采集层解耦。
 */

#include "net_info.hpp"
#include "serializer.hpp"
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

/**
 * @brief 二进制序列化格式版本号
 *
 * 写入缓冲区头部，fromBinary 读取时校验版本，不匹配则拒绝解析，
 * 从而实现格式向后兼容。首次发布版本为 1。
 */
constexpr int32_t kBinaryFormatVersion = 1;

/**
 * @brief 将任意 POD 类型按字节追加到缓冲区
 *
 * 使用 reinterpret_cast 将 POD 对象地址转为 uint8_t*，直接拷贝 sizeof(T) 字节。
 * 通过 static_assert 确保模板参数 T 是 trivially_copyable 的，避免 memcpy 风险。
 *
 * @tparam T POD 类型（trivially copyable）
 * @param value 要序列化的值
 * @param out  [out] 目标字节缓冲区
 */
template <typename T>
void appendBytes(const T& value, std::vector<uint8_t>& out) {
    static_assert(std::is_trivially_copyable<T>::value, "T must be trivially copyable");
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&value);
    out.insert(out.end(), p, p + sizeof(T));
}

/**
 * @brief 从指定 offset 处读取一个 POD 类型
 *
 * 从 buffer 的 offset 位置开始 memcpy sizeof(T) 字节到 out，
 * 成功后推进 offset。边界越界（offset + sizeof(T) > buffer.size()）返回 false。
 *
 * @tparam T POD 类型（trivially copyable）
 * @param buffer 字节缓冲区
 * @param offset [in,out] 当前读取位置，成功后自动推进
 * @param out    [out] 反序列化输出值
 *
 * @return true  - 读取成功
 *         false - 缓冲区越界
 */
template <typename T>
bool readBytes(const std::vector<uint8_t>& buffer, size_t& offset, T& out) {
    static_assert(std::is_trivially_copyable<T>::value, "T must be trivially copyable");
    if (offset + sizeof(T) > buffer.size()) return false;
    std::memcpy(&out, buffer.data() + offset, sizeof(T));
    offset += sizeof(T);
    return true;
}

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

/**
 * @brief 判断两个 NetInfo 是否有实质差异（用于决定是否需要对外推送更新）
 *
 * 先通过 sameKey 判断是否属于同一接口对象（ifname_ 相同），
 * 再通过 equals 比较关键字段（ifname/is_default/type/rtt/state），
 * 最后逐一比较动态采集指标（RSSI、丢包率、流量、抖动、蓝牙、频段冲突等）。
 *
 * @param other 另一个 NetInfo 对象
 *
 * @return true  - 有变化，需要通知下游
 */
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

/**
 * @brief 将 NetInfo 序列化为自定义二进制格式
 *
 * 格式（按顺序）：
 * 1. int32 版本号（kBinaryFormatVersion）
 * 2. serializeString(ifname_)
 * 3. int32 is_default (0/1)
 * 4. int32 type_（NetType 枚举转 int）
 * 5. int32 state_（NetState 枚举转 int）
 * 6. int32 using_now_ (0/1)
 * 7. int32 quality_（LinkQuality 枚举转 int）
 * 8. int32 rtt_ms_
 * 9. int32 prev_rtt_ms_
 * 10. int32 rssi_dbm_
 * 11. double tcp_loss_rate_（IEEE-754 原样存储）
 * 12. double jitter_ms_
 * 13. serializeString(tcp_loss_level_)
 * 14. serializeString(jitter_level_)
 * 15. uint64 traffic_total_bps_
 * 16. uint64 traffic_total_pps_
 * 17. uint32 traffic_active_flows_
 * 18. double bt_distance_
 * 19. serializeString(bt_audio_quality_)
 * 20. int32 band_conflict_ (0/1)
 * 21. double band_conflict_confidence_
 *
 * @return 包含完整二进制数据的 vector<uint8_t>
 *
 * @note 序列化顺序与 fromBinary 的反序列化顺序严格一致；
 *       serializeString 由 serializer.hpp 提供（length-prefixed 编码）。
 */
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

/**
 * @brief 从二进制缓冲区反序列化 NetInfo
 *
 * 工作流程：
 * 1. 读取并校验版本号，不匹配立即返回 false
 * 2. 按 toBinary 完全一致的顺序逐个读取字段
 * 3. 每步读取失败（缓冲区越界 / 枚举值越界）立即返回 false
 * 4. 全程写入临时对象 tmp，最后 *this = std::move(tmp) 提交
 *
 * @param buffer 二进制数据缓冲区（通常由 toBinary 生成）
 *
 * @return true  - 完整解析成功并提交到 this
 *         false - 缓冲区为空、版本不匹配、或任意字段解析失败
 */
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
