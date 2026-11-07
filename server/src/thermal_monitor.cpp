/**
 * @file thermal_monitor.cpp
 * @brief 采集端热状态观测与归因实现
 *
 * 分两层，刻意不合并：
 *   - ThermalStateReader  —— 只做 I/O，把 sysfs 变成 ThermalSnapshot
 *   - attributeThermalState —— 纯函数，只吃 ThermalSnapshot，可完全离线单测
 *
 * 为什么必须切开：热状态的判定规则（节流 / 风扇策略异常 / 正常）是该模块唯一
 * 有业务含义的部分，也是最容易写错的部分。如果它和 sysfs 读取混在一起，单元
 * 测试就只能跑在真实板子上——而真实板子恰好处于「未节流」的稳定态，永远测不到
 * 节流分支。切开后，节流、低温满载、高温停转全部能用构造的快照覆盖。
 */

#include "thermal_monitor.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <sstream>

namespace weaknet {

namespace {

/// 读一个只含数字（可能带前导空白与结尾换行）的 sysfs 文件。
/// 失败返回 false 且不修改 *out——**绝不用 0 冒充读失败**。
bool readLongFile(const std::string& path, long* out) {
    std::ifstream in(path);
    if (!in.is_open()) return false;
    long v = 0;
    if (!(in >> v)) return false;
    if (out) *out = v;
    return true;
}

bool readStringFile(const std::string& path, std::string* out) {
    std::ifstream in(path);
    if (!in.is_open()) return false;
    std::string s;
    if (!std::getline(in, s)) return false;
    // sysfs 的 type/name 文件带结尾换行，去掉后再比较，否则 "pwmfan\n" != "pwmfan"
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) {
        s.pop_back();
    }
    if (out) *out = s;
    return true;
}

/// 列出目录下所有以 prefix 开头的条目名，排序后返回。
///
/// 排序不是为了好看：readdir 的顺序由文件系统决定，不排序会让
/// 「最高温取哪个 zone」在同样输入下产生不同结果，单元测试会随机失败。
std::vector<std::string> listDirsWithPrefix(const std::string& root, const std::string& prefix) {
    std::vector<std::string> out;
    DIR* d = ::opendir(root.c_str());
    if (!d) return out;
    struct dirent* e = nullptr;
    while ((e = ::readdir(d)) != nullptr) {
        const std::string name = e->d_name;
        if (name.rfind(prefix, 0) != 0) continue;
        out.push_back(name);
    }
    ::closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// 读取层
// ---------------------------------------------------------------------------

ThermalStateReader::ThermalStateReader(std::string thermal_root,
                                       std::string hwmon_root,
                                       std::string cpufreq_root)
    : thermal_root_(std::move(thermal_root)),
      hwmon_root_(std::move(hwmon_root)),
      cpufreq_root_(std::move(cpufreq_root)) {}

ThermalSnapshot ThermalStateReader::read() const {
    ThermalSnapshot s;

    // ---- 1. 热区：/sys/class/thermal/thermal_zoneN/{type,temp} ----
    //
    // 不写死 thermal_zone0：不同 SoC 的分区命名与数量都不一样，写死会在换平台
    // 时静默读错分区（读到的是 CPU 小核温度却当成整机温度）。
    const auto zone_dirs = listDirsWithPrefix(thermal_root_, "thermal_zone");
    if (zone_dirs.empty()) {
        s.reason = "no_thermal_zones";
    } else {
        for (const auto& dir : zone_dirs) {
            const std::string base = thermal_root_ + "/" + dir;
            std::string type;
            long milli = 0;
            // type 读不出来不致命（退化成目录名，仍可溯源）；
            // temp 读不出来必须整条丢弃——补一个 0℃ 会污染最高温计算。
            if (!readLongFile(base + "/temp", &milli)) continue;
            if (!readStringFile(base + "/type", &type)) type = dir;

            ThermalZoneReading r;
            r.type = type;
            r.temp_c = static_cast<double>(milli) / 1000.0;
            s.zones.push_back(std::move(r));
        }

        if (s.zones.empty()) {
            s.reason = "all_zones_invalid";
        } else {
            s.available = true;
            // 用 std::max_element 而不是循环比较再取最后一个：
            // 温度相同时必须稳定取同一个 zone，否则 hottest_zone 会随
            // readdir 顺序漂移（虽然已排序，但显式保证更稳）。
            auto it = std::max_element(
                s.zones.begin(), s.zones.end(),
                [](const ThermalZoneReading& a, const ThermalZoneReading& b) {
                    return a.temp_c < b.temp_c;
                });
            s.max_temp_c = it->temp_c;
            s.hottest_zone = it->type;
        }
    }

    // ---- 2. 风扇：/sys/class/hwmon/hwmonN/{name,pwm1} ----
    //
    // 必须按 name == "pwmfan" 匹配，不能按 hwmonN 的序号：
    // hwmon 编号由内核按注册顺序分配，重启/驱动加载顺序变化时会整体位移，
    // 写死 hwmon8 看一眼能用，换个内核版本就指向了某个温度传感器的目录。
    const auto hwmon_dirs = listDirsWithPrefix(hwmon_root_, "hwmon");
    for (const auto& dir : hwmon_dirs) {
        const std::string base = hwmon_root_ + "/" + dir;
        std::string name;
        if (!readStringFile(base + "/name", &name)) continue;
        if (name != "pwmfan") continue;

        long pwm = 0;
        if (!readLongFile(base + "/pwm1", &pwm)) break;
        s.fan_present = true;
        // pwm1 的满量程在内核 hwmon ABI 里恒为 255，与 cooling device 的
        // max_state（本板为 4）是两套刻度，混用会让占空比算出 63.75 这种值。
        double duty = static_cast<double>(pwm) / 255.0;
        if (duty < 0.0) duty = 0.0;
        if (duty > 1.0) duty = 1.0;
        s.fan_duty_ratio = duty;
        break;
    }

    // ---- 3. CPU 频率上限：/sys/devices/system/cpu/cpufreq/policyN ----
    //
    // 见头文件对 scaling_max_freq 的说明：它是 thermal 节流唯一可靠信号。
    const auto policy_dirs = listDirsWithPrefix(cpufreq_root_, "policy");
    if (!policy_dirs.empty()) {
        long sum_max = 0;
        long sum_hwmax = 0;
        bool any = false;
        for (const auto& dir : policy_dirs) {
            const std::string base = cpufreq_root_ + "/" + dir;
            long cur_max = 0, hw_max = 0;
            if (!readLongFile(base + "/scaling_max_freq", &cur_max)) continue;
            if (!readLongFile(base + "/cpuinfo_max_freq", &hw_max)) continue;
            if (hw_max <= 0) continue;
            sum_max += cur_max;
            sum_hwmax += hw_max;
            any = true;
        }
        if (any && sum_hwmax > 0) {
            s.cpufreq_present = true;
            double ratio = static_cast<double>(sum_max) / static_cast<double>(sum_hwmax);
            if (ratio < 0.0) ratio = 0.0;
            if (ratio > 1.0) ratio = 1.0;
            s.freq_ceiling_ratio = ratio;
        }
    }

    // 注意：available 只在上面的热区分支里被置 true。温度完全读不到时，
    // 即便风扇与频率都读到了，本快照仍然是 unavailable——判定温度与风扇的
    // 匹配关系必须有温度，缺温度就只剩半个判断：报 NORMAL 会掩盖问题，
    // 报异常又没有依据。宁可说"不知道"。
    return s;
}

// ---------------------------------------------------------------------------
// 判定层（纯函数）
// ---------------------------------------------------------------------------

ThermalAttributionResult attributeThermalState(const ThermalSnapshot& snapshot,
                                               const ThermalThresholds& th) {
    ThermalAttributionResult res;

    // ---- 1. 没有数据 → 老实说不知道 ----
    //
    // 与 attributeObserverDegradation 的 UNATTRIBUTED 分支同构：
    // 缺数据时既不假设正常也不假设异常。这里的 evaluated=true 表示
    // 「确实尝试判定了」，与「模块被配置关闭所以没判」是两件事。
    if (!snapshot.available) {
        res.evaluated = true;
        res.attribution = ThermalAttribution::UNAVAILABLE;
        res.explanation =
            "Host thermal state is unavailable (" + snapshot.reason + "); "
            "cannot judge whether the collector is thermally throttled";
        return res;
    }

    res.evaluated = true;
    res.max_temp_c = snapshot.max_temp_c;
    res.fan_present = snapshot.fan_present;
    res.fan_duty_ratio = snapshot.fan_duty_ratio;
    res.cpufreq_present = snapshot.cpufreq_present;
    res.freq_ceiling_ratio = snapshot.freq_ceiling_ratio;

    // ---- 2. 收集问题（可多条并存）----
    bool throttled = false;
    if (snapshot.cpufreq_present &&
        snapshot.freq_ceiling_ratio < (1.0 - th.throttle_epsilon)) {
        throttled = true;
        res.issues.push_back("CPU_THROTTLED");
    }

    bool fan_anomaly = false;
    if (snapshot.fan_present) {
        if (snapshot.max_temp_c < th.low_temp_full_speed_c &&
            snapshot.fan_duty_ratio >= th.full_speed_duty) {
            fan_anomaly = true;
            res.issues.push_back("LOW_TEMP_FULL_SPEED");
        } else if (snapshot.max_temp_c >= th.high_temp_no_fan_c &&
                   snapshot.fan_duty_ratio <= th.no_fan_duty) {
            fan_anomaly = true;
            res.issues.push_back("HIGH_TEMP_NO_FAN");
        }
    }

    // ---- 3. 定主结论 ----
    //
    // 优先级：THROTTLED > FAN_ANOMALY > NORMAL
    // 理由见头文件：节流直接影响"这一轮观测可不可信"，是诊断链路要的信息；
    // 风扇策略异常只影响设备寿命预期。两者同时发生时，诊断链路先要前者。
    if (throttled) {
        res.attribution = ThermalAttribution::THROTTLED;
        std::ostringstream oss;
        oss << "Collector host CPU frequency ceiling is reduced to "
            << static_cast<int>(snapshot.freq_ceiling_ratio * 100.0)
            << "% of hardware max (hottest zone " << snapshot.hottest_zone
            << " at " << static_cast<int>(snapshot.max_temp_c) << " C); "
            << "thermal throttling can starve the collector of CPU and make "
            << "evidence loss self-inflicted rather than network-caused";
        if (fan_anomaly) {
            oss << "; fan state is also anomalous (" << res.issues.back() << ")";
        }
        res.explanation = oss.str();
        return res;
    }

    if (fan_anomaly) {
        res.attribution = ThermalAttribution::FAN_ANOMALY;
        std::ostringstream oss;
        oss << "Fan cooling policy is anomalous: hottest zone " << snapshot.hottest_zone
            << " at " << static_cast<int>(snapshot.max_temp_c) << " C while fan duty is "
            << static_cast<int>(snapshot.fan_duty_ratio * 100.0) << "% ("
            << res.issues.back() << "); "
            << "CPU frequency ceiling is normal at "
            << static_cast<int>(snapshot.freq_ceiling_ratio * 100.0)
            << "%, so this does not affect observation quality";
        res.explanation = oss.str();
        return res;
    }

    res.attribution = ThermalAttribution::NORMAL;
    {
        std::ostringstream oss;
        oss << "Host thermal state is normal: hottest zone " << snapshot.hottest_zone
            << " at " << static_cast<int>(snapshot.max_temp_c) << " C";
        if (snapshot.cpufreq_present) {
            oss << ", frequency ceiling at "
                << static_cast<int>(snapshot.freq_ceiling_ratio * 100.0) << "% of hardware max";
        }
        if (snapshot.fan_present) {
            oss << ", fan duty " << static_cast<int>(snapshot.fan_duty_ratio * 100.0) << "%";
        }
        res.explanation = oss.str();
    }
    return res;
}

}  // namespace weaknet
