#pragma once

#include "assurance/health_state.hpp"
#include "assurance/dns_types.hpp"
#include "assurance/observer_attribution.hpp"
#include "assurance/thermal_attribution.hpp"
#include "video_qoe_attribution.hpp"
#include <string>
#include <vector>
#include <optional>

namespace weaknet {

struct NetworkExperience {
    std::string iface;
    HealthState overall{HealthState::UNKNOWN};
    Coverage overall_coverage{Coverage::NONE};
    AssessmentProfile assessment_profile{AssessmentProfile::INTERNET_ACCESS}; // IR-3

    SleResult ip_reachability;
    SleResult responsiveness;
    SleResult reliability;
    SleResult rf_health;   // Advisory
    SleResult dns_service;   // Phase 2A: DNS Service SLE
    SleResult tcp_connect;    // Stage 2: TCP Connect SLE
    SleResult tcp_rtt;        // Passive TCP RTT (trace_tcp_probe srtt) SLE
    SleResult http_access;    // Stage 2: HTTP/HTTPS Access SLE
    SleResult captive_portal; // Stage 2: Captive Portal SLE

    // 受控主动探测（host-level Internet capability）——唯一有资格
    // 判定 INTERNET_ACCESS 的服务层证据。与被动 observed experience 分开表达。
    SleResult active_dns;
    SleResult active_tcp;
    SleResult active_https;
    SleResult active_portal;

    std::vector<std::string> warnings;
    std::optional<std::string> primary_issue;
    int display_score{50}; // 仅供展示与兼容映射

    /**
     * @brief 观测者降级归因（HOST 域证据的消费者）
     *
     * 当某个 SLE 报 observer 降级时，这里记录"为什么会降级"：
     *   SELF_INFLICTED      → 采集端 CPU 过载，证据丢失很可能是自己造成的
     *   NOT_HOST_CPU_BOUND  → CPU 正常，排除了过载这一个成因
     *   UNATTRIBUTED        → 没有 CPU 数据，老老实实说不知道
     *   NONE                → 本轮没有任何 SLE 报 observer 降级
     *
     * 重要边界：本字段**不参与** OverallPolicy 的评分与 veto。
     * 过载是本机运行状态，不是被评估链路的健康度。它的作用是让排障
     * 能区分"网络真的坏了"和"板子自己忙不过来"，而不是改变结论。
     *
     * 默认 NONE：在没有任何 observer 降级时，与引入该字段前行为完全一致。
     */
    ObserverAttribution observer_attribution{ObserverAttribution::NONE};

    /// 归因的人类可读解释（NONE 时为空）
    std::string observer_attribution_detail;

    /**
     * @brief 采集端热状态归因（同一轮评估的热域证据）
     *
     * observer_attribution 回答"CPU 有没有被任务占满"，本字段回答
     * "CPU 有没有被硬件限频"。两者是不同的成因，不能互相替代：
     *   THROTTLED     → 频率上限被 cpufreq cooling device 压低，采集端受限
     *   FAN_ANOMALY   → 温度与风扇匹配异常，只影响设备寿命，不影响观测质量
     *   NORMAL        → 温度正常、未节流
     *   UNAVAILABLE   → 读不到 sysfs，不猜
     *   NONE          → 本轮没评估（上一轮快照的默认值）
     *
     * 边界同 observer_attribution：**不参与** OverallPolicy 的评分与 veto。
     */
    ThermalAttribution thermal_attribution{ThermalAttribution::NONE};

    /// 归因的人类可读解释（NONE 时为空）
    std::string thermal_attribution_detail;

    /// 机器可读问题清单；允许"既节流又风扇异常"这类组合同时存在
    std::vector<std::string> thermal_issues;

    /**
     * @brief 视频 QoE 归因（业务侧体验证据）
     *
     * 与 observer_attribution / thermal_attribution 并列的第三种归因，
     * 回答一个前两者都答不了的问题：「用户看到的画面到底卡不卡」。
     *
     * 前两者看的是采集端自身状态（CPU 忙/热），本字段看的是业务侧真实体验。
     * 三者互不替代：
     *   CONSISTENT_HEALTHY   → 画面好、网络好，没有可归因的问题
     *   APP_LAYER_SUSPECT    → 画面差但网络好，查编码器/ISP/CPU
     *   NETWORK_TRANSIENT    → 画面差网络也差但未持续，先观察
     *   CONFIRMED_LINK_FAULT → 画面差网络差且持续，链路故障高置信
     *   NETWORK_UNVERIFIED   → 画面好但网络指标差，反向校验：可能我们自己观测错了
     *   NONE                 → 无帧统计（无摄像头 / 未启用），没有结论
     *
     * 边界同前两者：**不参与** OverallPolicy 的评分与 veto。
     *
     * 默认 NONE：在没有任何帧源接入时，与引入该字段前行为完全一致。
     */
    VideoQoeAttribution video_qoe_attribution{VideoQoeAttribution::NONE};

    /// 归因的人类可读解释（NONE 时为空）
    std::string video_qoe_attribution_detail;

    /// 机器可读问题清单（如 SEQ_GAP_BURST / CORRUPT_FRAMES）
    std::vector<std::string> video_qoe_issues;
};

} // namespace weaknet
