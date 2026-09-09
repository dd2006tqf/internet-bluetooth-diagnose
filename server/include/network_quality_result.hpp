#pragma once

#include <string>
#include <vector>
#include "net_info.hpp"

namespace weaknet_dbus {

/// 网络质量等级兼容枚举（CR-1, CR-3）
// 数值契约（与历史版本一致，禁止变更）：UNKNOWN=0, POOR=1, FAIR=2, GOOD=3, EXCELLENT=4；
// 数值单调递增表示质量变好，JSON quality_level 字段直接输出该数值。
enum class NetworkQualityLevel {
    UNKNOWN   = 0, ///< 未知
    POOR      = 1, ///< 差
    FAIR      = 2, ///< 一般
    GOOD      = 3, ///< 良好
    EXCELLENT = 4  ///< 优秀
};

/// 网络质量评估兼容结果结构体（供 D-Bus / DatabaseManager / Web 历史使用）
struct NetworkQualityResult {
    NetworkQualityLevel level{NetworkQualityLevel::UNKNOWN}; ///< 质量等级枚举
    std::string levelName{"UNKNOWN"};                        ///< 等级名称字符串
    std::string details;                                    ///< JSON 格式详细指标
    double score{50.0};                                     ///< 0-100 的兼容展示分数
    std::vector<std::string> issues;                        ///< 发现的问题/警告列表

    std::string overallQualityName() const { return levelName; }
};

} // namespace weaknet_dbus
