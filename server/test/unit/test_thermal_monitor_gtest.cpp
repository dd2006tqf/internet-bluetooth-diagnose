/**
 * @file test_thermal_monitor_gtest.cpp
 * @brief 采集端热状态：sysfs 读取真值表 + 归因判定真值表
 *
 * 为什么这些用例必须存在、且不能只在真机上跑：
 *   真实板子长期处在「28℃ / 未节流」这一个稳定点上，节流分支、高温停转分支
 *   在硬件上**永远走不到**。如果判定逻辑和 sysfs 读取混在一起，这两条分支
 *   就只能是"看起来对"的死代码。所以读取层用临时目录构造场景，
 *   判定层用构造的快照直接喂纯函数。
 *
 * 钉死的三条纪律：
 *   1. 读不到就是读不到 —— 不得用 0℃ / 0% 冒充，0 会被下游读成"很凉快"。
 *   2. 节流只看 scaling_max_freq —— scaling_cur_freq 低是省电，不是节流。
 *   3. 缺温度 → UNAVAILABLE，不猜正常也不猜异常。
 */

#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "thermal_monitor.hpp"

using namespace weaknet;

namespace {

/**
 * @brief 一次性临时目录树，析构时递归删除
 *
 * 不用 std::filesystem：本项目 C++17 目标是 bullseye 的 GCC 10，
 * 足够，但测试只做"建几层目录 + 写几个小文件"，用 POSIX 调用更直接，
 * 也避免 fs 实现的额外链接依赖。
 */
class TempTree {
public:
    TempTree() {
        char tmpl[] = "/tmp/weaknet-thermal-XXXXXX";
        char* d = ::mkdtemp(tmpl);
        EXPECT_NE(d, nullptr);
        if (d) root_ = d;
    }

    ~TempTree() {
        if (!root_.empty()) removeAll(root_);
    }

    TempTree(const TempTree&) = delete;
    TempTree& operator=(const TempTree&) = delete;

    const std::string& root() const { return root_; }

    /// 建目录（含中间层），返回绝对路径
    std::string dir(const std::string& rel) const {
        const std::string p = root_ + "/" + rel;
        std::string acc = root_;
        size_t i = 0;
        while (i <= rel.size()) {
            const size_t slash = rel.find('/', i);
            const std::string part =
                rel.substr(i, slash == std::string::npos ? std::string::npos : slash - i);
            if (!part.empty()) {
                acc += "/" + part;
                ::mkdir(acc.c_str(), 0755);
            }
            if (slash == std::string::npos) break;
            i = slash + 1;
        }
        return p;
    }

    /// 写文件（父目录需已存在）
    void write(const std::string& rel, const std::string& content) const {
        std::ofstream out(root_ + "/" + rel);
        ASSERT_TRUE(out.is_open()) << "cannot write " << rel;
        out << content;
    }

private:
    static void removeAll(const std::string& path) {
        // 只有测试用的浅层目录树，两遍足够：先删文件再删目录。
        std::string cmd = "rm -rf '" + path + "'";
        if (std::system(cmd.c_str()) != 0) {
            // 清理失败不影响断言结论，但要让使用者看得见
            std::fprintf(stderr, "warning: failed to clean temp dir %s\n", path.c_str());
        }
    }

    std::string root_;
};

/// 造一个"可用"的快照基线，各用例只改自己关心的维度
ThermalSnapshot makeSnapshot(double max_temp_c, bool fan_present, double fan_duty,
                             bool cpufreq_present, double freq_ratio) {
    ThermalSnapshot s;
    s.available = true;
    s.max_temp_c = max_temp_c;
    s.hottest_zone = "cpul_thermal_zone";
    s.zones.push_back({"cpul_thermal_zone", max_temp_c});
    s.fan_present = fan_present;
    s.fan_duty_ratio = fan_duty;
    s.cpufreq_present = cpufreq_present;
    s.freq_ceiling_ratio = freq_ratio;
    return s;
}

bool hasIssue(const ThermalAttributionResult& r, const std::string& id) {
    for (const auto& i : r.issues) {
        if (i == id) return true;
    }
    return false;
}

}  // namespace

// ===========================================================================
// 第一部分：sysfs 读取层
// ===========================================================================

TEST(ThermalRead, NoThermalZonesYieldsUnavailableNotZero) {
    TempTree t;
    t.dir("thermal");
    t.dir("hwmon");
    t.dir("cpufreq");

    ThermalStateReader reader(t.root() + "/thermal", t.root() + "/hwmon",
                              t.root() + "/cpufreq");
    const auto s = reader.read();

    // 0℃ 会让下游认为"非常凉快"，这是最危险的假数据
    EXPECT_FALSE(s.available);
    EXPECT_EQ(s.reason, "no_thermal_zones");
    EXPECT_TRUE(s.zones.empty());
    EXPECT_DOUBLE_EQ(s.max_temp_c, 0.0) << "0 只是默认值，必须靠 available=false 拦截";
}

TEST(ThermalRead, ZonesWithoutTemperatureAreInvalidNotZero) {
    TempTree t;
    t.dir("thermal/thermal_zone0");  // 有目录，没有 temp
    t.dir("hwmon");
    t.dir("cpufreq");

    ThermalStateReader reader(t.root() + "/thermal", t.root() + "/hwmon",
                              t.root() + "/cpufreq");
    const auto s = reader.read();

    EXPECT_FALSE(s.available);
    EXPECT_EQ(s.reason, "all_zones_invalid");
    EXPECT_TRUE(s.zones.empty()) << "读不到温度的 zone 必须整条丢弃，不得补 0";
}

TEST(ThermalRead, PicksHottestZoneNotZone0) {
    TempTree t;
    t.dir("thermal/thermal_zone0");
    t.write("thermal/thermal_zone0/type", "skin_zone\n");
    t.write("thermal/thermal_zone0/temp", "30000\n");
    t.dir("thermal/thermal_zone1");
    t.write("thermal/thermal_zone1/type", "cpul_thermal_zone\n");
    t.write("thermal/thermal_zone1/temp", "61250\n");
    t.dir("hwmon");
    t.dir("cpufreq");

    ThermalStateReader reader(t.root() + "/thermal", t.root() + "/hwmon",
                              t.root() + "/cpufreq");
    const auto s = reader.read();

    ASSERT_TRUE(s.available);
    EXPECT_EQ(s.zones.size(), 2u);
    // 写死 zone0 会读到 30℃ 而漏掉真正最热的 61.25℃
    EXPECT_DOUBLE_EQ(s.max_temp_c, 61.25);
    EXPECT_EQ(s.hottest_zone, "cpul_thermal_zone");
}

TEST(ThermalRead, UnreadableZoneTempDoesNotPolluteMax) {
    TempTree t;
    t.dir("thermal/thermal_zone0");
    t.write("thermal/thermal_zone0/type", "cpul_thermal_zone\n");
    t.write("thermal/thermal_zone0/temp", "28400\n");
    t.dir("thermal/thermal_zone1");
    t.write("thermal/thermal_zone1/type", "skin_zone\n");  // 无 temp
    t.dir("hwmon");
    t.dir("cpufreq");

    ThermalStateReader reader(t.root() + "/thermal", t.root() + "/hwmon",
                              t.root() + "/cpufreq");
    const auto s = reader.read();

    ASSERT_TRUE(s.available);
    EXPECT_EQ(s.zones.size(), 1u);
    EXPECT_DOUBLE_EQ(s.max_temp_c, 28.4);
    EXPECT_EQ(s.hottest_zone, "cpul_thermal_zone");
}

TEST(ThermalRead, FanMatchedByNameNotByIndex) {
    TempTree t;
    t.dir("thermal/thermal_zone0");
    t.write("thermal/thermal_zone0/type", "cpul_thermal_zone\n");
    t.write("thermal/thermal_zone0/temp", "30000\n");

    // hwmon0 在前但它是别的设备；按序号匹配会读到它
    t.dir("hwmon/hwmon0");
    t.write("hwmon/hwmon0/name", "soc_thermal\n");
    t.write("hwmon/hwmon0/pwm1", "0\n");
    t.dir("hwmon/hwmon1");
    t.write("hwmon/hwmon1/name", "pwmfan\n");
    t.write("hwmon/hwmon1/pwm1", "255\n");

    t.dir("cpufreq");

    ThermalStateReader reader(t.root() + "/thermal", t.root() + "/hwmon",
                              t.root() + "/cpufreq");
    const auto s = reader.read();

    ASSERT_TRUE(s.available);
    EXPECT_TRUE(s.fan_present);
    EXPECT_DOUBLE_EQ(s.fan_duty_ratio, 1.0) << "必须跟着 name==pwmfan 走";
}

TEST(ThermalRead, FanDutyUsesHwmonFullScaleNotCoolingMaxState) {
    TempTree t;
    t.dir("thermal/thermal_zone0");
    t.write("thermal/thermal_zone0/type", "cpul_thermal_zone\n");
    t.write("thermal/thermal_zone0/temp", "30000\n");
    t.dir("hwmon/hwmon0");
    t.write("hwmon/hwmon0/name", "pwmfan\n");
    t.write("hwmon/hwmon0/pwm1", "64\n");  // 64/255 ≈ 25.1%
    t.dir("cpufreq");

    ThermalStateReader reader(t.root() + "/thermal", t.root() + "/hwmon",
                              t.root() + "/cpufreq");
    const auto s = reader.read();

    ASSERT_TRUE(s.available);
    ASSERT_TRUE(s.fan_present);
    // 若误按 cooling device 的 max_state=4 算，会得到 64/4 = 16 这种荒谬值
    EXPECT_NEAR(s.fan_duty_ratio, 64.0 / 255.0, 1e-9);
}

TEST(ThermalRead, ThrottleRatioSumsScalingMaxOverCpuinfoMax) {
    TempTree t;
    t.dir("thermal/thermal_zone0");
    t.write("thermal/thermal_zone0/type", "cpul_thermal_zone\n");
    t.write("thermal/thermal_zone0/temp", "70000\n");
    t.dir("hwmon");

    t.dir("cpufreq/policy0");
    t.write("cpufreq/policy0/scaling_max_freq", "1000000\n");
    t.write("cpufreq/policy0/cpuinfo_max_freq", "2000000\n");
    t.dir("cpufreq/policy1");
    t.write("cpufreq/policy1/scaling_max_freq", "1000000\n");
    t.write("cpufreq/policy1/cpuinfo_max_freq", "2000000\n");

    ThermalStateReader reader(t.root() + "/thermal", t.root() + "/hwmon",
                              t.root() + "/cpufreq");
    const auto s = reader.read();

    ASSERT_TRUE(s.available);
    ASSERT_TRUE(s.cpufreq_present);
    EXPECT_DOUBLE_EQ(s.freq_ceiling_ratio, 0.5);
    EXPECT_EQ(s.cpufreq_present, true);
}

TEST(ThermalRead, LowScalingCurFreqIsNotThrottling) {
    TempTree t;
    t.dir("thermal/thermal_zone0");
    t.write("thermal/thermal_zone0/type", "cpul_thermal_zone\n");
    t.write("thermal/thermal_zone0/temp", "30000\n");
    t.dir("hwmon");

    // 本板 ondemand 空闲时就跑 416MHz，但上限从未被改写
    t.dir("cpufreq/policy0");
    t.write("cpufreq/policy0/scaling_cur_freq", "416000\n");
    t.write("cpufreq/policy0/scaling_max_freq", "2002000\n");
    t.write("cpufreq/policy0/cpuinfo_max_freq", "2002000\n");

    ThermalStateReader reader(t.root() + "/thermal", t.root() + "/hwmon",
                              t.root() + "/cpufreq");
    const auto s = reader.read();

    ASSERT_TRUE(s.available);
    ASSERT_TRUE(s.cpufreq_present);
    // 这一条是整个设计的支点：用 scaling_cur_freq 判节流会把省电当成故障
    EXPECT_DOUBLE_EQ(s.freq_ceiling_ratio, 1.0);
    // 频率低但上限没被压 → 必须判 NORMAL，绝不判 THROTTLED
    EXPECT_EQ(attributeThermalState(s).attribution, ThermalAttribution::NORMAL);
}

TEST(ThermalRead, FanAndFreqWithoutTemperatureStaysUnavailable) {
    TempTree t;
    t.dir("thermal");  // 没有热区
    t.dir("hwmon/hwmon0");
    t.write("hwmon/hwmon0/name", "pwmfan\n");
    t.write("hwmon/hwmon0/pwm1", "255\n");
    t.dir("cpufreq/policy0");
    t.write("cpufreq/policy0/scaling_max_freq", "2002000\n");
    t.write("cpufreq/policy0/cpuinfo_max_freq", "2002000\n");

    ThermalStateReader reader(t.root() + "/thermal", t.root() + "/hwmon",
                              t.root() + "/cpufreq");
    const auto s = reader.read();

    // 半边数据不足以判定：没有温度就没有"温度 vs 风扇"的匹配关系可判
    EXPECT_FALSE(s.available);
    EXPECT_TRUE(s.fan_present) << "读到的事实要保留，只是不能据此下结论";
    EXPECT_TRUE(s.cpufreq_present);
    EXPECT_EQ(attributeThermalState(s).attribution, ThermalAttribution::UNAVAILABLE);
}

TEST(ThermalRead, MissingFanIsNotAnError) {
    TempTree t;
    t.dir("thermal/thermal_zone0");
    t.write("thermal/thermal_zone0/type", "cpul_thermal_zone\n");
    t.write("thermal/thermal_zone0/temp", "50000\n");
    t.dir("hwmon");
    t.dir("cpufreq/policy0");
    t.write("cpufreq/policy0/scaling_max_freq", "2002000\n");
    t.write("cpufreq/policy0/cpuinfo_max_freq", "2002000\n");

    ThermalStateReader reader(t.root() + "/thermal", t.root() + "/hwmon",
                              t.root() + "/cpufreq");
    const auto s = reader.read();

    ASSERT_TRUE(s.available);
    EXPECT_FALSE(s.fan_present);
    EXPECT_DOUBLE_EQ(s.fan_duty_ratio, 0.0);
    // 无风扇设备不是异常，判定不得因此报 FAN_ANOMALY
    EXPECT_EQ(attributeThermalState(s).attribution, ThermalAttribution::NORMAL);
}

// ===========================================================================
// 第二部分：归因判定层（纯函数）
// ===========================================================================

TEST(ThermalAttribution, UnavailableIsReportedAsUnknownNotNormal) {
    ThermalSnapshot s;
    s.reason = "no_thermal_zones";

    const auto r = attributeThermalState(s);

    EXPECT_EQ(r.attribution, ThermalAttribution::UNAVAILABLE);
    EXPECT_TRUE(r.evaluated) << "尝试判定过，只是判不出结果";
    EXPECT_FALSE(r.explanation.empty());
    EXPECT_TRUE(r.issues.empty());
    // 不得因为"不知道"就宣称正常
    EXPECT_NE(r.attribution, ThermalAttribution::NORMAL);
}

TEST(ThermalAttribution, NormalWhenCoolAndNotThrottled) {
    const auto r = attributeThermalState(
        makeSnapshot(55.0, /*fan*/ true, 0.30, /*cpufreq*/ true, 1.0));

    EXPECT_EQ(r.attribution, ThermalAttribution::NORMAL);
    EXPECT_TRUE(r.evaluated);
    EXPECT_TRUE(r.issues.empty());
    EXPECT_FALSE(r.explanation.empty());
    EXPECT_DOUBLE_EQ(r.max_temp_c, 55.0);
    EXPECT_DOUBLE_EQ(r.freq_ceiling_ratio, 1.0);
}

TEST(ThermalAttribution, ThrottledWhenFrequencyCeilingReduced) {
    const auto r = attributeThermalState(
        makeSnapshot(88.0, /*fan*/ true, 0.80, /*cpufreq*/ true, 0.50));

    EXPECT_EQ(r.attribution, ThermalAttribution::THROTTLED);
    EXPECT_TRUE(hasIssue(r, "CPU_THROTTLED"));
    EXPECT_FALSE(hasIssue(r, "HIGH_TEMP_NO_FAN")) << "85℃ 风扇 80% 不算停转";
    EXPECT_NE(r.explanation.find("self-inflicted"), std::string::npos)
        << "解释必须点明'证据丢失可能不是网络造成的'";
}

TEST(ThermalAttribution, ThrottledOutranksFanAnomalyButKeepsBothIssues) {
    // 既节流又低温满载：这是本板 + 被压低上限的组合，完全可能同时发生
    const auto r = attributeThermalState(
        makeSnapshot(28.4, /*fan*/ true, 1.0, /*cpufreq*/ true, 0.50));

    // 主结论取影响观测可信度的那个
    EXPECT_EQ(r.attribution, ThermalAttribution::THROTTLED);
    // 但两条问题都不能丢
    ASSERT_EQ(r.issues.size(), 2u);
    EXPECT_TRUE(hasIssue(r, "CPU_THROTTLED"));
    EXPECT_TRUE(hasIssue(r, "LOW_TEMP_FULL_SPEED"));
}

TEST(ThermalAttribution, LowTempFullSpeedIsFanAnomaly) {
    // 本板实测工况：28.4℃ 而 pwm1=255/255
    const auto r = attributeThermalState(
        makeSnapshot(28.4, /*fan*/ true, 1.0, /*cpufreq*/ true, 1.0));

    EXPECT_EQ(r.attribution, ThermalAttribution::FAN_ANOMALY);
    EXPECT_TRUE(hasIssue(r, "LOW_TEMP_FULL_SPEED"));
    EXPECT_EQ(r.issues.size(), 1u);
    EXPECT_NE(r.explanation.find("does not affect observation quality"), std::string::npos)
        << "风扇策略异常是设备可靠性问题，必须明说不影响观测质量";
}

TEST(ThermalAttribution, HighTempNoFanIsFanAnomaly) {
    const auto r = attributeThermalState(
        makeSnapshot(92.0, /*fan*/ true, 0.05, /*cpufreq*/ true, 1.0));

    EXPECT_EQ(r.attribution, ThermalAttribution::FAN_ANOMALY);
    EXPECT_TRUE(hasIssue(r, "HIGH_TEMP_NO_FAN"));
}

TEST(ThermalAttribution, FanAnomalyDoesNotFakeThrottling) {
    const auto r = attributeThermalState(
        makeSnapshot(28.4, /*fan*/ true, 1.0, /*cpufreq*/ true, 1.0));

    // 频率上限没被压低 → 绝不能报 THROTTLED
    EXPECT_NE(r.attribution, ThermalAttribution::THROTTLED);
    EXPECT_FALSE(hasIssue(r, "CPU_THROTTLED"));
}

TEST(ThermalAttribution, NoFanDeviceNeverReportsFanAnomaly) {
    const auto r = attributeThermalState(
        makeSnapshot(92.0, /*fan*/ false, 0.0, /*cpufreq*/ true, 1.0));

    EXPECT_EQ(r.attribution, ThermalAttribution::NORMAL);
    EXPECT_TRUE(r.issues.empty());
}

TEST(ThermalAttribution, ThresholdsAreBoundaryInclusiveAsDocumented) {
    // 45.0℃ / 90% 恰好落在边界：45℃ 已经不"低于"阈值，不算异常
    const auto at_edge = attributeThermalState(
        makeSnapshot(45.0, true, 0.90, true, 1.0));
    EXPECT_EQ(at_edge.attribution, ThermalAttribution::NORMAL);

    // 85.0℃ / 10% 恰好落在边界：>= 与 <= 都成立，算停转
    const auto at_hot_edge = attributeThermalState(
        makeSnapshot(85.0, true, 0.10, true, 1.0));
    EXPECT_EQ(at_hot_edge.attribution, ThermalAttribution::FAN_ANOMALY);
    EXPECT_TRUE(hasIssue(at_hot_edge, "HIGH_TEMP_NO_FAN"));
}

TEST(ThermalAttribution, ThrottleEpsilonAbsorbsSysfsRounding) {
    // 0.995 的偏差在 1% 死区内，不得因取整误差每轮横跳
    const auto in_deadband = attributeThermalState(
        makeSnapshot(50.0, true, 0.30, true, 0.995));
    EXPECT_EQ(in_deadband.attribution, ThermalAttribution::NORMAL);

    // 0.98 已超出死区，必须判节流
    const auto out_of_deadband = attributeThermalState(
        makeSnapshot(50.0, true, 0.30, true, 0.98));
    EXPECT_EQ(out_of_deadband.attribution, ThermalAttribution::THROTTLED);
}

TEST(ThermalAttribution, MissingCpufreqSkipsThrottleCheck) {
    // 没读到 cpufreq 不等于"没节流"，但也不能凭默认值 1.0 之外的东西报节流
    ThermalSnapshot s = makeSnapshot(50.0, true, 0.30, /*cpufreq*/ false, 0.10);
    const auto r = attributeThermalState(s);

    EXPECT_FALSE(r.cpufreq_present);
    EXPECT_EQ(r.attribution, ThermalAttribution::NORMAL)
        << "频率维度缺失时不得据其默认值下结论";
}

TEST(ThermalAttribution, ResultCarriesRawValuesForEvidence) {
    const auto r = attributeThermalState(
        makeSnapshot(61.25, true, 0.42, true, 0.75));

    // 归因结论必须可溯源：原始读数原样带出，不得二次加工
    EXPECT_DOUBLE_EQ(r.max_temp_c, 61.25);
    EXPECT_DOUBLE_EQ(r.fan_duty_ratio, 0.42);
    EXPECT_TRUE(r.fan_present);
    EXPECT_DOUBLE_EQ(r.freq_ceiling_ratio, 0.75);
    EXPECT_TRUE(r.cpufreq_present);
}

TEST(ThermalAttribution, UnavailableIsStableAcrossRepeatedCalls) {
    // 与 observer_attribution 同款守护：读不到数据时反复调用必须稳定报不可用
    ThermalSnapshot s;
    s.reason = "all_zones_invalid";

    for (int i = 0; i < 3; ++i) {
        const auto r = attributeThermalState(s);
        EXPECT_EQ(r.attribution, ThermalAttribution::UNAVAILABLE) << "round " << i;
        EXPECT_TRUE(r.evaluated) << "round " << i;
    }
}
