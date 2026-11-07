/**
 * @file thermal_monitor.hpp
 * @brief 采集端热状态观测与归因：区分「网络坏了」与「采集端自己跑不动」
 *
 * ============================================================================
 * 要解决的漏洞
 * ============================================================================
 *
 * observer_attribution.hpp 已经能把「eBPF 掉证据」分辨成两类：
 *   (a) 采集端 CPU 被任务占满 → SELF_INFLICTED
 *   (b) CPU 正常 → NOT_HOST_CPU_BOUND
 *
 * 但它只看得见「忙」，看不见「为什么忙 / 忙了也没用」。热节流是第三种成因：
 * CPU 不是被任务占满，而是被**硬件限频**——`scaling_max_freq` 被 cpufreq
 * cooling device 压低，同样的负载在更低的频率上只能完成更少的工作。
 *
 * 三者对运维的含义截然不同：
 *   - 任务占满  → 查业务进程
 *   - 网络故障  → 查链路
 *   - 热节流    → 查散热/机柜环境
 *
 * 归错一个，现场就会在错误的方向上排查一整天。
 *
 * ============================================================================
 * 第二个业务作用：设备健康度本身
 * ============================================================================
 *
 * 工业现场的网关常年装在配电柜、机柜这类散热恶劣的地方。本板实测存在一个
 * 真实的策略异常：`cpul_thermal_zone = 28.4℃` 而 `pwm-fan` 满载
 * （`pwm1 = 255/255`）——IPA 的 `sustainable_power` 只有 756 mW，远低于实际
 * 功耗，于是它认为「必须全力散热」。这不是故障，是参数未校准，但后果是
 * 噪音 + 功耗 + 轴承寿命损耗，属于现场真实痛点。
 *
 * **纪律**：本模块只读、只判、只报。不写任何 thermal sysfs、不注册 cooling
 * device、不参与 IPA、不改 OverallPolicy 的任何阈值。
 *
 * ============================================================================
 * 语义边界（不得越过）
 * ============================================================================
 *
 * - 热节流 **不能**推出"网络是好的"。它只能说明"观测者自己受限"。
 * - 温度正常 **不能**推出"网络坏了"。它只能排除"热节流"这一个成因。
 * - 归因结论永远不参与 OverallPolicy 的评分与 veto，只作为证据呈现。
 *   理由同 observer_attribution：散热是本机的物理状态，不是被评估链路的健康度。
 * - 读不到 sysfs 时必须报 UNAVAILABLE，**不得**当成"温度正常"或"温度 0"。
 */

#pragma once

#include "assurance/thermal_attribution.hpp"

#include <string>
#include <vector>

namespace weaknet {

/**
 * @brief 单个热区的读数
 */
struct ThermalZoneReading {
    /// sysfs `type` 原文（如 "cpul_thermal_zone"、"skin_zone"）
    std::string type;
    /// 摄氏度。sysfs 原始单位是毫摄氏度，读取时已换算。
    double temp_c{0.0};
};

/**
 * @brief 一次热状态采样
 *
 * 三个维度各自独立可用：温度、风扇、频率上限。任一维度读不到不影响其他维度，
 * 因为它们在判定中的角色不同（温度决定异常方向，风扇与频率决定异常形态）。
 */
struct ThermalSnapshot {
    /// 至少读到一个热度数才算可用
    bool available{false};

    /// available=false 时的机器可读原因：
    ///   "no_thermal_zones"  /sys/class/thermal 为空或不可读
    ///   "all_zones_invalid" 目录存在但每一个 temp 都读不出来
    std::string reason;

    std::vector<ThermalZoneReading> zones;

    /// 全部热区中的最高温（摄氏度）与对应的 type。
    /// 用最高温而不是某个固定 zone：不同 SoC 的热区命名与数量都不同，
    /// 写死 zone0 会在换平台时静默读错分区。
    double max_temp_c{0.0};
    std::string hottest_zone;

    // ---- 风扇（hwmon pwmfan）----
    bool fan_present{false};
    /// pwm1 / 255，0~1。hwmon 的 pwm1 满量程恒为 255（内核约定），
    /// 不是 max_state——后者是 cooling device 的挡位序号，两者不可混用。
    double fan_duty_ratio{0.0};

    // ---- CPU 频率上限（热节流的直接证据）----
    bool cpufreq_present{false};
    /**
     * @brief 频率上限被压低的比例对应的剩余比例：sum(scaling_max_freq) /
     *        sum(cpuinfo_max_freq)（按每个 policy 求和）。
     *
     * 1.0 表示所有 policy 的上限都等于硬件上限 → 未被节流。
     * 0.5 表示上限被压到一半 → 正在节流。
     *
     * 为什么用 scaling_max_freq 而不是 scaling_cur_freq：
     *   `scaling_cur_freq` 低既可能是"降频省电"（ondemand 在空闲时就这么干，
     *   本板实测 policy6 空闲时 416 MHz）也可能是"被限频"，两者同形。
     *   而 `scaling_max_freq` 只由 cpufreq cooling device 改写——
     *   thermal 节流的实现方式正是压低这个上限。它是**唯一**能区分
     *   "省电降频"与"被迫降频"的可靠信号。
     */
    double freq_ceiling_ratio{1.0};
};

// ThermalAttribution / thermalAttributionToString 定义在
// assurance/thermal_attribution.hpp —— 那是评估快照层唯一需要依赖的部分。
// 若把它们留在这里，network_experience.hpp 为了拿到一个枚举就得把整个
// sysfs 读取器（<fstream>/<dirent.h>）拖进快照序列化层。

/**
 * @brief 判定阈值
 *
 * 默认值来自本板实测与工业机柜的常见工作区间，不是随意拍的：
 *   - 45℃：低于此温度还满载风扇，说明冷却策略过度，属于策略问题而非散热需求；
 *   - 85℃：多数 ARM SoC 的被动降频 trip 点在 85~95℃ 之间，高于此温度风扇仍
 *           接近停转，说明风扇/驱动失效；
 *   - 0.90 / 0.10：留出 10% 的余量，避免 PWM 抖动在边界反复横跳。
 */
struct ThermalThresholds {
    double low_temp_full_speed_c{45.0};
    double full_speed_duty{0.90};
    double high_temp_no_fan_c{85.0};
    double no_fan_duty{0.10};
    /// 上限比例低于 (1 - epsilon) 才算节流；1% 的死区吸收 sysfs 的取整误差
    double throttle_epsilon{0.01};
};

/**
 * @brief 归因结果
 */
struct ThermalAttributionResult {
    ThermalAttribution attribution{ThermalAttribution::NONE};

    /// 是否真的做了一次评估（false 表示本轮没有热状态数据可判）
    bool evaluated{false};

    /// 人类可读解释（进 warnings / experience JSON 的 detail 字段）
    std::string explanation;

    /// 机器可读的问题清单（可同时有多条：既节流又风扇异常是允许的组合）。
    /// 主 attribution 只取其中对观测质量影响最大的一个，但清单保留全部，
    /// 否则"风扇坏了但没节流"这种情况会因为 enum 只能存一个值而丢失。
    std::vector<std::string> issues;

    /// 参与判定的原始值，便于 evidence 溯源（不做任何加工）
    double max_temp_c{0.0};
    double fan_duty_ratio{0.0};
    bool fan_present{false};
    double freq_ceiling_ratio{1.0};
    bool cpufreq_present{false};
};

/**
 * @brief 热状态归因（无状态纯函数，可单测）
 *
 * 判定顺序（先影响业务的，再影响可靠性的）：
 *   1. snapshot 不可用            → UNAVAILABLE，evaluated=true，不猜
 *   2. 频率上限被压低              → THROTTLED（可叠加 FAN_ANOMALY 到 issues）
 *   3. 温度/风扇匹配异常           → FAN_ANOMALY
 *   4. 其余                        → NORMAL
 *
 * 为什么 THROTTLED 优先于 FAN_ANOMALY：前者直接影响"这一轮观测可不可信"，
 * 是诊断链路要用的信息；后者只影响设备寿命预期，是运维要用的信息。
 * 两者都发生时，诊断链路先要知道前者——但 issues 里两条都会保留。
 *
 * @param snapshot 采样结果
 * @param th       阈值
 */
ThermalAttributionResult attributeThermalState(
    const ThermalSnapshot& snapshot,
    const ThermalThresholds& th = ThermalThresholds{});

/**
 * @brief 从 sysfs 读取一次热状态
 *
 * 纯读取，无副作用。每次调用都重新 open/read/close：
 *   - 这些文件是内核动态生成的虚拟文件，单次读约 10~50 µs；
 *   - 调用方是 15 秒一轮的评估循环，缓存 fd 省下的时间可以忽略；
 *   - 不缓存意味着 thermal zone 热插拔（极少见但存在）时不会读到陈旧数据。
 *
 * 所有路径都可由参数覆盖，唯一目的是让单元测试能用固定目录构造场景，
 * 而不需要伪造 /sys——真实路径作为默认值，生产代码零配置。
 */
class ThermalStateReader {
public:
    explicit ThermalStateReader(
        std::string thermal_root = "/sys/class/thermal",
        std::string hwmon_root = "/sys/class/hwmon",
        std::string cpufreq_root = "/sys/devices/system/cpu/cpufreq");

    ThermalSnapshot read() const;

private:
    std::string thermal_root_;
    std::string hwmon_root_;
    std::string cpufreq_root_;
};

}  // namespace weaknet
