// test_quality_assessor_gtest.cpp
// Network Quality Assurance & LegacyAdapter compatibility unit tests
// Validates CR-1, CR-2, CR-3 and Legacy mapping integrity

#include <gtest/gtest.h>
#include "assurance/legacy_adapter.hpp"
#include "assurance/overall_policy.hpp"
#include "net_info.hpp"
#include <chrono>

using namespace weaknet;
using namespace weaknet_dbus;

class QualityAssessorCompatTest : public ::testing::Test {
protected:
    static NetworkExperience makeExp(HealthState overall_st, int score, const std::string& iface = "wlan0") {
        NetworkExperience exp;
        exp.iface = iface;
        exp.overall = overall_st;
        exp.display_score = score;
        exp.overall_coverage = Coverage::FULL_FOR_PROFILE;
        if (overall_st == HealthState::BAD) {
            exp.primary_issue = "Critical network failure";
        }
        return exp;
    }
};

// Test 1: Excellent mapping (GOOD + display_score >= 90) -> EXCELLENT (4)
TEST_F(QualityAssessorCompatTest, ExcellentMapping) {
    auto exp = makeExp(HealthState::GOOD, 95);
    auto res = LegacyAdapter::toQualityResult(exp);

    EXPECT_EQ(res.level, NetworkQualityLevel::EXCELLENT);
    EXPECT_EQ(static_cast<int>(res.level), 4);
    EXPECT_EQ(res.levelName, "EXCELLENT");
    EXPECT_GE(res.score, 90.0);

    std::string json = LegacyAdapter::toHealthCheckJson(exp);
    EXPECT_NE(json.find("\"overall_quality\":\"EXCELLENT\""), std::string::npos);
    EXPECT_NE(json.find("\"quality_level\":4"), std::string::npos);
    EXPECT_NE(json.find("\"score_model\":\"assurance_v2\""), std::string::npos);
    EXPECT_NE(json.find("\"rf_applicability\":\"APPLICABLE\""), std::string::npos);
}

// Test 2: Good mapping (GOOD + display_score < 90) -> GOOD (3)
TEST_F(QualityAssessorCompatTest, GoodMapping) {
    auto exp = makeExp(HealthState::GOOD, 85);
    auto res = LegacyAdapter::toQualityResult(exp);

    EXPECT_EQ(res.level, NetworkQualityLevel::GOOD);
    EXPECT_EQ(static_cast<int>(res.level), 3);
    EXPECT_EQ(res.levelName, "GOOD");
    EXPECT_DOUBLE_EQ(res.score, 85.0);

    std::string json = LegacyAdapter::toHealthCheckJson(exp);
    EXPECT_NE(json.find("\"overall_quality\":\"GOOD\""), std::string::npos);
    EXPECT_NE(json.find("\"quality_level\":3"), std::string::npos);
}

// Test 3: Degraded mapping -> FAIR (2)
TEST_F(QualityAssessorCompatTest, DegradedMapping) {
    auto exp = makeExp(HealthState::DEGRADED, 65);
    auto res = LegacyAdapter::toQualityResult(exp);

    EXPECT_EQ(res.level, NetworkQualityLevel::FAIR);
    EXPECT_EQ(static_cast<int>(res.level), 2);
    EXPECT_EQ(res.levelName, "FAIR");
    EXPECT_DOUBLE_EQ(res.score, 65.0);

    std::string json = LegacyAdapter::toHealthCheckJson(exp);
    EXPECT_NE(json.find("\"overall_quality\":\"FAIR\""), std::string::npos);
    EXPECT_NE(json.find("\"quality_level\":2"), std::string::npos);
}

// Test 4: Bad mapping -> POOR (1)
TEST_F(QualityAssessorCompatTest, BadMapping) {
    auto exp = makeExp(HealthState::BAD, 20);
    auto res = LegacyAdapter::toQualityResult(exp);

    EXPECT_EQ(res.level, NetworkQualityLevel::POOR);
    EXPECT_EQ(static_cast<int>(res.level), 1);
    EXPECT_EQ(res.levelName, "POOR");
    EXPECT_DOUBLE_EQ(res.score, 20.0);
    EXPECT_FALSE(res.issues.empty());

    std::string json = LegacyAdapter::toHealthCheckJson(exp);
    EXPECT_NE(json.find("\"overall_quality\":\"POOR\""), std::string::npos);
    EXPECT_NE(json.find("\"quality_level\":1"), std::string::npos);
}

// Test 5: Unknown mapping -> UNKNOWN (0)
TEST_F(QualityAssessorCompatTest, UnknownMapping) {
    auto exp = makeExp(HealthState::UNKNOWN, 50);
    auto res = LegacyAdapter::toQualityResult(exp);

    EXPECT_EQ(res.level, NetworkQualityLevel::UNKNOWN);
    EXPECT_EQ(static_cast<int>(res.level), 0);
    EXPECT_EQ(res.levelName, "UNKNOWN");

    std::string json = LegacyAdapter::toHealthCheckJson(exp);
    EXPECT_NE(json.find("\"overall_quality\":\"UNKNOWN\""), std::string::npos);
    EXPECT_NE(json.find("\"quality_level\":0"), std::string::npos);
}

// Test 6: Legacy level names match expectations
TEST_F(QualityAssessorCompatTest, QualityLevelNames) {
    EXPECT_EQ(LegacyAdapter::toLegacyLevelName(NetworkQualityLevel::EXCELLENT), "EXCELLENT");
    EXPECT_EQ(LegacyAdapter::toLegacyLevelName(NetworkQualityLevel::GOOD), "GOOD");
    EXPECT_EQ(LegacyAdapter::toLegacyLevelName(NetworkQualityLevel::FAIR), "FAIR");
    EXPECT_EQ(LegacyAdapter::toLegacyLevelName(NetworkQualityLevel::POOR), "POOR");
    EXPECT_EQ(LegacyAdapter::toLegacyLevelName(NetworkQualityLevel::UNKNOWN), "UNKNOWN");
}
