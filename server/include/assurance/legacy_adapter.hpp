#pragma once

#include "assurance/network_experience.hpp"
#include "network_quality_result.hpp"
#include <string>
#include <sstream>
#include <iomanip>

namespace weaknet {

/**
 * @brief 唯一无状态兼容适配器 (LegacyAdapter)
 *
 * CR-1, CR-2, CR-3: 将内部唯一的 NetworkExperience 映射为：
 * 1. 旧 NetworkQualityResult (供 DatabaseManager & 单元测试)
 * 2. 旧 HealthCheck JSON 响应 (供 D-Bus HealthCheck)
 * 3. 旧 NetworkQualityChanged 信号事件载荷 (供 EventManager)
 */
class LegacyAdapter {
public:
    static weaknet_dbus::NetworkQualityLevel toLegacyLevel(const NetworkExperience& exp) {
        switch (exp.overall) {
            case HealthState::GOOD:
                return (exp.display_score >= 90) ? weaknet_dbus::NetworkQualityLevel::EXCELLENT
                                                 : weaknet_dbus::NetworkQualityLevel::GOOD;
            case HealthState::DEGRADED:
                return weaknet_dbus::NetworkQualityLevel::FAIR;
            case HealthState::BAD:
                return weaknet_dbus::NetworkQualityLevel::POOR;
            case HealthState::UNKNOWN:
            default:
                return weaknet_dbus::NetworkQualityLevel::UNKNOWN;
        }
    }

    static std::string toLegacyLevelName(weaknet_dbus::NetworkQualityLevel level) {
        switch (level) {
            case weaknet_dbus::NetworkQualityLevel::EXCELLENT: return "EXCELLENT";
            case weaknet_dbus::NetworkQualityLevel::GOOD: return "GOOD";
            case weaknet_dbus::NetworkQualityLevel::FAIR: return "FAIR";
            case weaknet_dbus::NetworkQualityLevel::POOR: return "POOR";
            case weaknet_dbus::NetworkQualityLevel::UNKNOWN:
            default: return "UNKNOWN";
        }
    }

    static std::string toHealthCheckJson(const NetworkExperience& exp,
                                        int rtt_ms = -1,
                                        double tcp_loss = 0.0,
                                        int rssi_dbm = -1000,
                                        double jitter_ms = 0.0,
                                        double median_rtt_ms = -1.0,
                                        const std::string& resp_reason = "") {
        weaknet_dbus::NetworkQualityLevel legacyLevel = toLegacyLevel(exp);
        std::string levelName = toLegacyLevelName(legacyLevel);

        std::ostringstream json;
        json << "{\"success\":true,\"data\":{";
        json << "\"interface\":\"" << exp.iface << "\",";
        json << "\"quality_score\":" << std::fixed << std::setprecision(1) << static_cast<double>(exp.display_score) << ",";
        json << "\"rtt_ms\":" << rtt_ms << ",";
        json << "\"tcp_loss_rate\":" << std::fixed << std::setprecision(2) << (tcp_loss >= 0 ? tcp_loss : 0.0) << ",";
        json << "\"rssi_dbm\":" << rssi_dbm << ",";
        json << "\"rssi_estimated\":false,";
        json << "\"rssi_source\":\"nl80211\",";
        json << "\"traffic_bps\":0,";
        json << "\"traffic_pps\":0,";
        json << "\"active_flows\":0,";
        json << "\"jitter_ms\":" << std::fixed << std::setprecision(1) << (jitter_ms >= 0.0 ? jitter_ms : 0.0) << ",";
        json << "\"median_rtt_ms\":" << std::fixed << std::setprecision(1) << median_rtt_ms << ",";
        json << "\"assessment_reason\":\"" << (resp_reason.empty() ? exp.responsiveness.reason : resp_reason) << "\",";
        json << "\"quality_level\":" << static_cast<int>(legacyLevel) << ",";
        json << "\"link_quality\":\"Good\",";
        json << "\"overall_quality\":\"" << levelName << "\",";
        json << "\"overall_score\":" << std::fixed << std::setprecision(1) << static_cast<double>(exp.display_score) << ",";
        json << "\"rf_applicability\":\"" << applicabilityToString(exp.rf_health.applicability) << "\",";
        json << "\"assessment_profile\":\"" << assessmentProfileToString(exp.assessment_profile) << "\",";

        json << "\"issues\":[";
        std::vector<std::string> all_issues = exp.warnings;
        if (exp.primary_issue.has_value()) {
            all_issues.insert(all_issues.begin(), exp.primary_issue.value());
        }
        for (size_t i = 0; i < all_issues.size(); ++i) {
            json << "\"" << all_issues[i] << "\"";
            if (i + 1 < all_issues.size()) json << ",";
        }
        json << "],";

        json << "\"score_model\":\"assurance_v2\"";
        json << "}}";
        return json.str();
    }

    // 保留 Phase 1 的旧结果适配接口；HealthCheck 主路径使用 assurance_v2。
    static weaknet_dbus::NetworkQualityResult toQualityResult(const NetworkExperience& exp,
                                                            int rtt_ms = -1,
                                                            double tcp_loss = 0.0,
                                                            int rssi_dbm = -1000,
                                                            double jitter_ms = 0.0,
                                                            double median_rtt_ms = -1.0) {
        weaknet_dbus::NetworkQualityResult res;
        res.level = toLegacyLevel(exp);
        res.levelName = toLegacyLevelName(res.level);
        res.score = static_cast<double>(exp.display_score);
        if (exp.primary_issue.has_value()) {
            res.issues.push_back(exp.primary_issue.value());
        }
        for (const auto& w : exp.warnings) {
            res.issues.push_back(w);
        }
        res.details = toHealthCheckJson(exp, rtt_ms, tcp_loss, rssi_dbm, jitter_ms, median_rtt_ms);
        return res;
    }

    static std::string toExperienceJsonV1(const NetworkExperience& exp) {
        std::ostringstream json;
        json << "{\"schema_version\":1,";
        json << "\"interface\":\"" << exp.iface << "\",";
        json << "\"overall\":{";
        json << "\"state\":\"" << healthStateToString(exp.overall) << "\",";
        json << "\"coverage\":\"" << coverageToString(exp.overall_coverage) << "\",";
        json << "\"display_score\":" << exp.display_score;
        json << "},";

        auto dump_sle = [&json](const char* key, const SleResult& sle) {
            json << "\"" << key << "\":{";
            json << "\"state\":\"" << healthStateToString(sle.state) << "\",";
            json << "\"coverage\":\"" << coverageToString(sle.coverage) << "\",";
            json << "\"applicability\":\"" << applicabilityToString(sle.applicability) << "\",";
            json << "\"reason\":\"" << sle.reason << "\",";
            json << "\"evidence\":[";
            for (size_t i = 0; i < sle.evidence.size(); ++i) {
                json << "{\"metric\":\"" << sle.evidence[i].metric << "\",\"value\":" << sle.evidence[i].value << ",\"detail\":\"" << sle.evidence[i].detail << "\"}";
                if (i + 1 < sle.evidence.size()) json << ",";
            }
            json << "]}";
        };

        json << "\"sle\":{";
        dump_sle("ip_reachability", exp.ip_reachability);
        json << ",";
        dump_sle("responsiveness", exp.responsiveness);
        json << ",";
        dump_sle("reliability", exp.reliability);
        json << ",";
        dump_sle("rf_health", exp.rf_health);
        json << "},";

        json << "\"warnings\":[";
        for (size_t i = 0; i < exp.warnings.size(); ++i) {
            json << "\"" << exp.warnings[i] << "\"";
            if (i + 1 < exp.warnings.size()) json << ",";
        }
        json << "],";

        json << "\"primary_issue\":" << (exp.primary_issue.has_value() ? ("\"" + exp.primary_issue.value() + "\"") : "null");
        json << "}";
        return json.str();
    }

    static std::string toExperienceJsonV2(const NetworkExperience& exp) {
        std::ostringstream json;
        json << "{\"schema_version\":2,";
        json << "\"interface\":\"" << exp.iface << "\",";
        json << "\"assessment_profile\":\"" << assessmentProfileToString(exp.assessment_profile) << "\",";
        json << "\"overall\":{";
        json << "\"state\":\"" << healthStateToString(exp.overall) << "\",";
        json << "\"coverage\":\"" << coverageToString(exp.overall_coverage) << "\",";
        json << "\"display_score\":" << exp.display_score;
        json << "},";

        auto dump_sle = [&json](const char* key, const SleResult& sle) {
            json << "\"" << key << "\":{";
            json << "\"state\":\"" << healthStateToString(sle.state) << "\",";
            json << "\"coverage\":\"" << coverageToString(sle.coverage) << "\",";
            json << "\"applicability\":\"" << applicabilityToString(sle.applicability) << "\",";
            json << "\"reason\":\"" << sle.reason << "\",";
            json << "\"evidence\":[";
            for (size_t i = 0; i < sle.evidence.size(); ++i) {
                json << "{\"metric\":\"" << sle.evidence[i].metric << "\",\"value\":" << sle.evidence[i].value << ",\"detail\":\"" << sle.evidence[i].detail << "\"}";
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
            json << "\"" << exp.warnings[i] << "\"";
            if (i + 1 < exp.warnings.size()) json << ",";
        }
        json << "],";

        json << "\"primary_issue\":" << (exp.primary_issue.has_value() ? ("\"" + exp.primary_issue.value() + "\"") : "null");
        json << "}";
        return json.str();
    }
};

} // namespace weaknet
