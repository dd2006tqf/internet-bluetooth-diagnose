/**
 * @file wireless_event.cpp
 * @brief 无线设备事件领域模型 — 枚举转换、HCI 原因映射与 JSON 序列化
 *
 * 本文件只做三件事：
 *   1. 枚举 <-> 字符串（供 DB 存储与 JSON 展示使用）
 *   2. 内核 HCI error code -> 归一化 DisconnectReason 的映射
 *   3. WirelessDeviceEvent / RawBtObservation 的 JSON 序列化
 *
 * 关于映射函数的重要约定：
 *   disconnectReasonFromHciCode() 返回 Unknown 表示"该 code 不在我们的分类表里"，
 *   调用方必须同时保存原始 code（raw_reason_code），否则 Other/Unknown 会成为信息黑洞。
 */

#include "wireless_event.hpp"

#include <cstdio>
#include <sstream>

#include "utils/json_escape.hpp"

namespace weaknet_dbus {

// ============================================================================
// 枚举 -> 字符串
//
// 命名风格：全大写下划线，与项目既有 assurance/dns_types.hpp 保持一致，
// 便于 DB 存储与跨语言（云端/前端）消费。
// ============================================================================

const char* toString(WirelessProtocol v) {
    switch (v) {
        case WirelessProtocol::Bluetooth: return "BLUETOOTH";
        case WirelessProtocol::Wifi:      return "WIFI";
        case WirelessProtocol::Zigbee:    return "ZIGBEE";
        case WirelessProtocol::Other:     return "OTHER";
    }
    return "UNKNOWN";
}

const char* toString(BtAddressType v) {
    switch (v) {
        case BtAddressType::Bredr:    return "BREDR";
        case BtAddressType::LePublic: return "LE_PUBLIC";
        case BtAddressType::LeRandom: return "LE_RANDOM";
        case BtAddressType::Unknown:  return "UNKNOWN";
    }
    return "UNKNOWN";
}

const char* toString(DeviceEventType v) {
    switch (v) {
        case DeviceEventType::DeviceAppeared:    return "DEVICE_APPEARED";
        case DeviceEventType::DeviceLost:        return "DEVICE_LOST";
        case DeviceEventType::LinkConnected:     return "LINK_CONNECTED";
        case DeviceEventType::LinkDisconnected:  return "LINK_DISCONNECTED";
        case DeviceEventType::LinkDegraded:      return "LINK_DEGRADED";
        case DeviceEventType::LinkRecovered:     return "LINK_RECOVERED";
        case DeviceEventType::ConnectionAttempt: return "CONNECTION_ATTEMPT";
    }
    return "UNKNOWN";
}

const char* toString(DisconnectReason v) {
    switch (v) {
        case DisconnectReason::Unknown:               return "UNKNOWN";
        case DisconnectReason::RemoteUserTerminated:  return "REMOTE_USER_TERMINATED";
        case DisconnectReason::ConnectionTimeout:     return "CONNECTION_TIMEOUT";
        case DisconnectReason::AuthenticationFailure: return "AUTHENTICATION_FAILURE";
        case DisconnectReason::LocalHostTerminated:   return "LOCAL_HOST_TERMINATED";
        case DisconnectReason::Other:                 return "OTHER";
    }
    return "UNKNOWN";
}

const char* toString(EvidenceSource v) {
    switch (v) {
        case EvidenceSource::Unknown:          return "UNKNOWN";
        case EvidenceSource::KernelMgmt:       return "KERNEL_MGMT";
        case EvidenceSource::KernelHci:        return "KERNEL_HCI";
        case EvidenceSource::KernelHciTimeout: return "KERNEL_HCI_TIMEOUT";
        case EvidenceSource::BluezDbus:        return "BLUEZ_DBUS";
        case EvidenceSource::Derived:          return "DERIVED";
    }
    return "UNKNOWN";
}

const char* toString(LinkQualityState v) {
    switch (v) {
        case LinkQualityState::Learning: return "LEARNING";
        case LinkQualityState::Stable:   return "STABLE";
        case LinkQualityState::Degraded: return "DEGRADED";
    }
    return "UNKNOWN";
}

// ============================================================================
// 字符串 -> 枚举
//
// 只用于解析由本模块自己写出的字符串（DB 回读、测试），因此对大小写敏感。
// 无法识别时返回调用方给的 fallback，不抛异常——历史数据里出现未知枚举值
// 不应该让整个查询失败。
// ============================================================================

WirelessProtocol wirelessProtocolFromString(const std::string& s, WirelessProtocol fallback) {
    if (s == "BLUETOOTH") return WirelessProtocol::Bluetooth;
    if (s == "WIFI")      return WirelessProtocol::Wifi;
    if (s == "ZIGBEE")    return WirelessProtocol::Zigbee;
    if (s == "OTHER")     return WirelessProtocol::Other;
    return fallback;
}

DeviceEventType deviceEventTypeFromString(const std::string& s, DeviceEventType fallback) {
    if (s == "DEVICE_APPEARED")    return DeviceEventType::DeviceAppeared;
    if (s == "DEVICE_LOST")        return DeviceEventType::DeviceLost;
    if (s == "LINK_CONNECTED")     return DeviceEventType::LinkConnected;
    if (s == "LINK_DISCONNECTED")  return DeviceEventType::LinkDisconnected;
    if (s == "LINK_DEGRADED")      return DeviceEventType::LinkDegraded;
    if (s == "LINK_RECOVERED")     return DeviceEventType::LinkRecovered;
    if (s == "CONNECTION_ATTEMPT") return DeviceEventType::ConnectionAttempt;
    return fallback;
}

DisconnectReason disconnectReasonFromString(const std::string& s, DisconnectReason fallback) {
    if (s == "UNKNOWN")                return DisconnectReason::Unknown;
    if (s == "REMOTE_USER_TERMINATED") return DisconnectReason::RemoteUserTerminated;
    if (s == "CONNECTION_TIMEOUT")     return DisconnectReason::ConnectionTimeout;
    if (s == "AUTHENTICATION_FAILURE") return DisconnectReason::AuthenticationFailure;
    if (s == "LOCAL_HOST_TERMINATED")  return DisconnectReason::LocalHostTerminated;
    if (s == "OTHER")                  return DisconnectReason::Other;
    return fallback;
}

LinkQualityState linkQualityStateFromString(const std::string& s, LinkQualityState fallback) {
    if (s == "LEARNING") return LinkQualityState::Learning;
    if (s == "STABLE")   return LinkQualityState::Stable;
    if (s == "DEGRADED") return LinkQualityState::Degraded;
    return fallback;
}

BtAddressType btAddressTypeFromString(const std::string& s, BtAddressType fallback) {
    if (s == "BREDR")     return BtAddressType::Bredr;
    if (s == "LE_PUBLIC") return BtAddressType::LePublic;
    if (s == "LE_RANDOM") return BtAddressType::LeRandom;
    if (s == "UNKNOWN")   return BtAddressType::Unknown;
    return fallback;
}

EvidenceSource evidenceSourceFromString(const std::string& s, EvidenceSource fallback) {
    if (s == "UNKNOWN")          return EvidenceSource::Unknown;
    if (s == "KERNEL_MGMT")      return EvidenceSource::KernelMgmt;
    if (s == "KERNEL_HCI")       return EvidenceSource::KernelHci;
    if (s == "KERNEL_HCI_TIMEOUT") return EvidenceSource::KernelHciTimeout;
    if (s == "BLUEZ_DBUS")       return EvidenceSource::BluezDbus;
    if (s == "DERIVED")          return EvidenceSource::Derived;
    return fallback;
}

// ============================================================================
// HCI error code -> 归一化断连原因
//
// 取值来源：Bluetooth Core Specification, Vol 2, Part D (Error Codes)。
// 只映射与"链路断开原因"语义直接相关的 code；HCI 的其它错误码（命令失败、
// 参数非法等）不属于断连原因域，一律归入 Other 并保留原始 code。
//
// 分类依据：
//   - 对端主动断开：0x13 Remote User Terminated / 0x14 Remote Device Terminated
//                   (Low Resources) / 0x15 Remote Device Terminated (Power Off)
//   - 链路超时：    0x08 Connection Timeout / 0x10 Connection Accept Timeout
//                   Exceeded / 0x22 LMP Response Timeout
//   - 认证失败：    0x05 Authentication Failure / 0x06 PIN or Key Missing
//                   / 0x25 Encryption Mode Not Acceptable / 0x2F Insufficient Security
//   - 本机断开：    0x16 Connection Terminated By Local Host
// ============================================================================

DisconnectReason disconnectReasonFromHciCode(uint8_t hci_code) {
    switch (hci_code) {
        // --- 对端主动断开 ---
        case 0x13:  // Remote User Terminated Connection
        case 0x14:  // Remote Device Terminated Connection due to Low Resources
        case 0x15:  // Remote Device Terminated Connection due to Power Off
            return DisconnectReason::RemoteUserTerminated;

        // --- 链路超时 ---
        case 0x08:  // Connection Timeout
        case 0x10:  // Connection Accept Timeout Exceeded
        case 0x22:  // LMP Response Timeout / LL Response Timeout
            return DisconnectReason::ConnectionTimeout;

        // --- 认证 / 配对失败 ---
        case 0x05:  // Authentication Failure
        case 0x06:  // PIN or Key Missing
        case 0x25:  // Encryption Mode Not Acceptable
        case 0x2F:  // Insufficient Security
            return DisconnectReason::AuthenticationFailure;

        // --- 本机主动断开 ---
        case 0x16:  // Connection Terminated By Local Host
            return DisconnectReason::LocalHostTerminated;

        default:
            // 未分类的 code 一律 Other；原始值由 raw_reason_code 无损保留
            return DisconnectReason::Other;
    }
}

// ============================================================================
// 内核 mgmt 层原因码 -> 归一化断连原因
//
// 取值来源：内核 include/net/bluetooth/mgmt.h 的 MGMT_DEV_DISCONN_*
// 由内核 hci_to_mgmt_reason() 从 HCI error code 转换而来。
// 与 HCI 域是**两套不同的取值空间**，不能混用映射函数。
// ============================================================================

DisconnectReason disconnectReasonFromMgmtCode(uint8_t mgmt_code) {
    switch (mgmt_code) {
        case 0x00:  // MGMT_DEV_DISCONN_UNKNOWN
            return DisconnectReason::Unknown;
        case 0x01:  // MGMT_DEV_DISCONN_TIMEOUT
            return DisconnectReason::ConnectionTimeout;
        case 0x02:  // MGMT_DEV_DISCONN_LOCAL_HOST
            return DisconnectReason::LocalHostTerminated;
        case 0x03:  // MGMT_DEV_DISCONN_REMOTE
            return DisconnectReason::RemoteUserTerminated;
        case 0x04:  // MGMT_DEV_DISCONN_AUTH_FAILURE
            return DisconnectReason::AuthenticationFailure;
        case 0x05:  // MGMT_DEV_DISCONN_LOCAL_HOST_SUSPEND（主机挂起导致的断开）
            return DisconnectReason::LocalHostTerminated;
        default:
            return DisconnectReason::Other;
    }
}

// ============================================================================
// 内核 link_type + LE 地址类型 -> BtAddressType
//
// 对照内核 net/bluetooth/mgmt.c 的 link_to_bdaddr()。
// 注意取值域是 HCI 域的 ADDR_LE_DEV_*（0=public,1=random,2=public_resolved,
// 3=random_resolved），不是 mgmt API 的 BDADDR_* 域。
// ============================================================================

BtAddressType btAddressTypeFromKernel(uint8_t link_type, uint8_t addr_type) {
    if (link_type != kKernelLeLink) {
        return BtAddressType::Bredr;   // SCC/ACL 等经典链路一律 BR/EDR
    }
    // LE：只有明确的 public 才是 LE Public；random 与 *_resolved 都归 LE Random
    // （resolved 地址实际承载的是随机可解析地址，内核 link_to_bdaddr 同样这样归类）
    return (addr_type == 0x00) ? BtAddressType::LePublic : BtAddressType::LeRandom;
}

// ============================================================================
// BDADDR 转换
// ============================================================================

std::string formatBdaddr(const uint8_t bdaddr[6]) {
    // 内核 bdaddr_t 反序存储：b[0] 是最低字节，显示时最高字节在前。
    // 对照 BlueZ ba2str() 的 b[5]..b[0] 打印顺序。
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             bdaddr[5], bdaddr[4], bdaddr[3], bdaddr[2], bdaddr[1], bdaddr[0]);
    return std::string(buf);
}

bool parseBdaddr(const std::string& mac, uint8_t out[6]) {
    unsigned int b[6] = {0};
    // 严格要求 "XX:XX:XX:XX:XX:XX"（17 字符），避免宽松解析把
    // "AA-BB-CC-DD-EE-FF" 之类格式悄悄接受后又按错误语义解释
    if (mac.size() != 17) return false;
    if (sscanf(mac.c_str(), "%2x:%2x:%2x:%2x:%2x:%2x",
               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; ++i) {
        if (b[i] > 0xFF) return false;
        // 反序填充，与内核 bdaddr_t 布局一致
        out[5 - i] = static_cast<uint8_t>(b[i]);
    }
    return true;
}

// ============================================================================
// WirelessDeviceKey
// ============================================================================

bool WirelessDeviceKey::operator<(const WirelessDeviceKey& o) const {
    if (site_id != o.site_id) return site_id < o.site_id;
    if (gateway_id != o.gateway_id) return gateway_id < o.gateway_id;
    if (hci_index != o.hci_index) return hci_index < o.hci_index;
    if (protocol != o.protocol) return protocol < o.protocol;
    if (address_type != o.address_type) return address_type < o.address_type;
    return device_address < o.device_address;
}

bool WirelessDeviceKey::operator==(const WirelessDeviceKey& o) const {
    return site_id == o.site_id &&
           gateway_id == o.gateway_id &&
           hci_index == o.hci_index &&
           protocol == o.protocol &&
           address_type == o.address_type &&
           device_address == o.device_address;
}

std::string WirelessDeviceKey::toString() const {
    std::ostringstream oss;
    oss << site_id << "/" << gateway_id << "/hci" << hci_index << "/"
        << weaknet_dbus::toString(protocol) << "/"
        << weaknet_dbus::toString(address_type) << "/"
        << device_address;
    return oss.str();
}

// ============================================================================
// DeviceLinkProfile::toJson
// ============================================================================

std::string DeviceLinkProfile::toJson() const {
    std::ostringstream oss;
    oss << "{";
    oss << "\"site_id\":\"" << weaknet_utils::escapeJsonString(key.site_id) << "\",";
    oss << "\"gateway_id\":\"" << weaknet_utils::escapeJsonString(key.gateway_id) << "\",";
    oss << "\"hci_index\":" << key.hci_index << ",";
    oss << "\"protocol\":\"" << weaknet_dbus::toString(key.protocol) << "\",";
    oss << "\"address_type\":\"" << weaknet_dbus::toString(key.address_type) << "\",";
    oss << "\"device_address\":\"" << weaknet_utils::escapeJsonString(key.device_address) << "\",";
    oss << "\"baseline_rssi_dbm\":";
    if (baseline_rssi_dbm.has_value()) oss << *baseline_rssi_dbm; else oss << "null";
    oss << ",\"min_seen_rssi_dbm\":";
    if (min_seen_rssi_dbm.has_value()) oss << *min_seen_rssi_dbm; else oss << "null";
    oss << ",\"max_seen_rssi_dbm\":";
    if (max_seen_rssi_dbm.has_value()) oss << *max_seen_rssi_dbm; else oss << "null";
    oss << ",\"baseline_sample_count\":" << baseline_sample_count << ",";
    oss << "\"first_seen_ms\":" << first_seen_ms << ",";
    oss << "\"last_seen_ms\":" << last_seen_ms << ",";
    oss << "\"state\":\"" << weaknet_dbus::toString(state) << "\",";
    oss << "\"updated_at_ms\":" << updated_at_ms;
    oss << "}";
    return oss.str();
}

// ============================================================================
// JSON 序列化
// ============================================================================

namespace {

/// 拼接可选 int：nullopt -> null，有值 -> 数字
void appendOptionalInt(std::ostringstream& oss, const std::optional<int>& v) {
    if (v.has_value()) {
        oss << *v;
    } else {
        oss << "null";
    }
}

/// 拼接可选 string：nullopt -> null，有值 -> "..."（已转义）
void appendOptionalString(std::ostringstream& oss, const std::optional<std::string>& v) {
    if (v.has_value()) {
        oss << "\"" << weaknet_utils::escapeJsonString(*v) << "\"";
    } else {
        oss << "null";
    }
}

}  // namespace

std::string WirelessDeviceEvent::toJson() const {
    std::ostringstream oss;
    oss << "{";
    oss << "\"event_id\":\"" << weaknet_utils::escapeJsonString(event_id) << "\",";
    oss << "\"site_id\":\"" << weaknet_utils::escapeJsonString(site_id) << "\",";
    oss << "\"gateway_id\":\"" << weaknet_utils::escapeJsonString(gateway_id) << "\",";
    oss << "\"hci_index\":" << hci_index << ",";
    oss << "\"protocol\":\"" << toString(protocol) << "\",";
    oss << "\"device_address\":\"" << weaknet_utils::escapeJsonString(device_address) << "\",";
    oss << "\"address_type\":\"" << toString(address_type) << "\",";
    oss << "\"event_type\":\"" << toString(event_type) << "\",";
    oss << "\"timestamp_ms\":" << timestamp_ms << ",";
    oss << "\"rssi_at_event_dbm\":";
    appendOptionalInt(oss, rssi_at_event_dbm);
    oss << ",";
    oss << "\"raw_reason_code\":" << static_cast<unsigned>(raw_reason_code) << ",";
    oss << "\"reason\":\"" << toString(reason) << "\",";
    oss << "\"source\":\"" << toString(source) << "\",";
    oss << "\"source_detail\":\"" << weaknet_utils::escapeJsonString(source_detail) << "\",";
    oss << "\"suspected_cause\":";
    appendOptionalString(oss, suspected_cause);
    oss << ",";
    oss << "\"details\":" << (details_json.empty() ? "{}" : details_json);
    oss << "}";
    return oss.str();
}

std::string appendRawEvidence(const std::string& details_json,
                              const RawBtObservation& obs) {
    std::ostringstream entry;
    entry << "{";
    entry << "\"ts\":" << obs.timestamp_ms << ",";
    entry << "\"hci_index\":" << obs.hci_index << ",";
    entry << "\"source\":\"" << toString(obs.source) << "\",";
    entry << "\"source_detail\":\"" << weaknet_utils::escapeJsonString(obs.source_detail) << "\",";
    entry << "\"event_type\":\"" << toString(obs.event_type) << "\",";
    entry << "\"raw_reason_code\":";
    if (obs.raw_reason_code.has_value()) {
        entry << static_cast<unsigned>(*obs.raw_reason_code);
    } else {
        entry << "null";
    }
    entry << ",\"reason_hint\":\"" << toString(obs.reason_hint) << "\"";
    entry << "}";

    // details_json 形如 {"raw_evidence":[...]}；为空或不是预期结构时重建容器。
    // 这里刻意做简单字符串操作而非引入 JSON 库——本字段只由本模块写入，
    // 结构可控，且 details_json 被明确定义为"审计性证据"而非查询关键数据。
    const std::string key = "\"raw_evidence\":[";
    if (details_json.empty()) {
        return "{" + key + entry.str() + "]}";
    }
    const size_t pos = details_json.find(key);
    if (pos == std::string::npos) {
        return "{" + key + entry.str() + "]}";
    }
    // 在数组的右括号处插入：找到数组起始后的第一个 ']'
    const size_t arr_start = pos + key.size();
    const size_t arr_end = details_json.find(']', arr_start);
    if (arr_end == std::string::npos) {
        return details_json;  // 结构异常，原样返回，不破坏已有数据
    }
    const bool empty_array = (arr_end == arr_start);
    std::string out = details_json;
    out.insert(arr_end, (empty_array ? "" : ",") + entry.str());
    return out;
}

}  // namespace weaknet_dbus
