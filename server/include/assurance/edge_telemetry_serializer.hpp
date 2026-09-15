#pragma once

/**
 * @file edge_telemetry_serializer.hpp
 * @brief 把 NetworkExperience 序列化为边缘上报契约（network.edge.telemetry.v1）
 *
 * ## 为什么单独一份，而不是复用 toExperienceJsonV2
 *
 * `LegacyAdapter::toExperienceJsonV2` 产出的是 **D-Bus 契约**（schema_version 2），
 * 其形状已被客户端与既有测试依赖（如 `"schema_version":2`、扁平
 * `overall`/`network_health`/`service_health`）。上报契约要求的是嵌套的
 * `snapshot` 对象，且 `overall` 拆成 `overall_state`/`overall_coverage`。
 *
 * 若在上报侧"先生成 v2 字符串再改写形状"，就变成对刚产出的 JSON 做字符串
 * 手术：既脆弱（字段顺序/空白稍变即失效），又让两份契约的实际内容互相
 * 污染。因此这里独立输出，D-Bus 契约保持逐字节不变。
 *
 * ## 转义
 *
 * 本函数对所有字符串字段做 RFC 8259 转义。既有 v2 实现直接内联拼接
 * （历史原因）；上报路径不能沿用——`iface` 来自系统、`reason`/`detail`
 * 可能含引号，未转义会直接产出非法 JSON，且签名覆盖的是这段字节，
 * 服务端会在"验签通过但解析失败"处卡住，难以定位。
 *
 * ## 字段命名
 *
 * 与 `network_assurance/contracts.py` 的 `NetworkExperienceSnapshot` 一一对应。
 * 新增字段必须两侧同时改，否则服务端 `extra="forbid"` 会拒绝整条上报。
 */

#include <sstream>
#include <string>

#include "assurance/dns_types.hpp"
#include "assurance/health_state.hpp"
#include "assurance/network_experience.hpp"
#include "utils/json_escape.hpp"

namespace weaknet {

/// 链路媒介类型：决定哪些诊断假设成立（有线链路无 RF 退化等）。
inline const char* edgeLinkTypeToString(const NetworkExperience& exp) {
    // 仅根据已有信息做保守判定：无法确知时返回 UNKNOWN，
    // 绝不用接口名猜频段（那会伪造出一份不存在的观测）。
    if (exp.rf_health.applicability == Applicability::NOT_APPLICABLE) {
        return "WIRED_ETHERNET";
    }
    if (exp.rf_health.applicability == Applicability::APPLICABLE) {
        return "WIFI_2_4G";  // 具体频段由上报方补充；此处给保守的 Wi-Fi 归类
    }
    return "UNKNOWN";
}

/**
 * @brief 序列化上报契约中的 `snapshot` 对象（不含外层信封）。
 *
 * 输出形如：
 *   {"interface":"wlan0","assessment_profile":"INTERNET_ACCESS",
 *    "overall_state":"DEGRADED","overall_coverage":"PARTIAL","display_score":55,
 *    "network_health":{...},"service_health":{...},
 *    "warnings":[...],"primary_issue":"...","link_type":"..."}
 */
inline std::string toEdgeTelemetrySnapshotJson(const NetworkExperience& exp) {
    using weaknet_utils::escapeJsonString;
    std::ostringstream json;

    json << "{";
    json << "\"interface\":\"" << escapeJsonString(exp.iface) << "\",";
    json << "\"assessment_profile\":\""
         << assessmentProfileToString(exp.assessment_profile) << "\",";
    json << "\"overall_state\":\"" << healthStateToString(exp.overall) << "\",";
    json << "\"overall_coverage\":\"" << coverageToString(exp.overall_coverage) << "\",";
    json << "\"display_score\":" << exp.display_score << ",";

    auto dump_sle = [&json](const char* key, const SleResult& sle) {
        json << "\"" << key << "\":{";
        json << "\"state\":\"" << healthStateToString(sle.state) << "\",";
        json << "\"coverage\":\"" << coverageToString(sle.coverage) << "\",";
        json << "\"applicability\":\"" << applicabilityToString(sle.applicability) << "\",";
        // capability_level_negative 必须跨语言携带：它是服务端判断
        // "能否据此处以否决上网能力"的唯一依据。丢失它会让服务端
        // 有机会给出比边缘更强的结论（把单域名失败当成主机能力失效）。
        json << "\"capability_level_negative\":"
             << (sle.capability_level_negative ? "true" : "false") << ",";
        json << "\"reason\":\"" << escapeJsonString(sle.reason) << "\",";
        json << "\"evidence\":[";
        for (size_t i = 0; i < sle.evidence.size(); ++i) {
            json << "{\"metric\":\"" << escapeJsonString(sle.evidence[i].metric) << "\","
                 << "\"value\":" << sle.evidence[i].value << ","
                 << "\"detail\":\"" << escapeJsonString(sle.evidence[i].detail) << "\"}";
            if (i + 1 < sle.evidence.size()) json << ",";
        }
        json << "]}";
    };

    json << "\"network_health\":{";
    dump_sle("ip_reachability", exp.ip_reachability);
    json << ",";
    dump_sle("responsiveness", exp.responsiveness);
    json << ",";
    dump_sle("reliability", exp.reliability);
    json << ",";
    dump_sle("rf_health", exp.rf_health);
    json << "},";

    json << "\"service_health\":{";
    dump_sle("dns", exp.dns_service);
    json << ",";
    dump_sle("tcp_connect", exp.tcp_connect);
    json << ",";
    dump_sle("http_access", exp.http_access);
    json << ",";
    dump_sle("captive_portal", exp.captive_portal);
    json << ",";
    dump_sle("active_dns", exp.active_dns);
    json << ",";
    dump_sle("active_tcp", exp.active_tcp);
    json << ",";
    dump_sle("active_https", exp.active_https);
    json << ",";
    dump_sle("active_portal", exp.active_portal);
    json << "},";

    json << "\"warnings\":[";
    for (size_t i = 0; i < exp.warnings.size(); ++i) {
        json << "\"" << escapeJsonString(exp.warnings[i]) << "\"";
        if (i + 1 < exp.warnings.size()) json << ",";
    }
    json << "],";

    json << "\"primary_issue\":"
         << (exp.primary_issue.has_value()
                 ? ("\"" + escapeJsonString(exp.primary_issue.value()) + "\"")
                 : "null");
    json << ",";

    json << "\"link_type\":\"" << edgeLinkTypeToString(exp) << "\"";
    // 说明：mac/ip/gateway/dns_servers/ap_* 等拓扑字段当前不在此处填充——
    // C++ 侧尚无采集实现（NetInfo 不暴露地址信息）。契约中它们均为可选，
    // 缺失即表示"本端未采集"，而不是"不存在"。补采集时在此追加，
    // 并在 contracts.py 同步字段。
    json << "}";
    return json.str();
}

}  // namespace weaknet
