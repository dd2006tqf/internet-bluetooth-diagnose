#pragma once

/**
 * @file edge_wireless_uplink_exporter.hpp
 * @brief 边缘无线事实上行器 — 把 device_events / site_incidents / device_baselines
 *        推送中心平台（Phase 4a）
 *
 * ## 与 EdgeTelemetryExporter 的关系
 *
 * 两者**并列**、各自独立线程与缓冲，刻意不合并：
 *   - 遥测按 `(network_epoch, sequence_id)` 幂等，是单调快照流；
 *   - 无线事实按 `event_id` / `incident_id` 幂等，是不可变事实 + 生命周期行。
 * 把两类幂等域塞进一个信封，会让"哪一条失败、该重发什么"的语义互相耦合
 * （见集成设计决策 D1）。
 *
 * ## 为什么数据源是 SQLite 而不是内存
 *
 * device_events / site_incidents 是**跨重启延续的事实**（关联器启动回放就是
 * 依赖它们），上行必须覆盖"网断了 10 分钟、期间发生的事故"这段历史。
 * 内存环形缓冲只保留最近 N 条，不足以承担补发。
 *
 * ## 游标与重放
 *
 * `data/wireless-uplink-watermark` 记录"已确认送达的最大事实时间"。
 * 游标只在 HTTP 2xx 之后前移；失败则原地重试——服务端按 id 幂等，
 * 因此重发是安全且常态的。游标文件损坏/缺失时从 0 重新扫（历史事实会
 * 被判重复，不会重复入库），绝不猜测一个"更靠后"的位置而漏发。
 *
 * ## 签名与序列化顺序
 *
 * 与遥测同一不变式：先序列化成 bytes → 对这段 bytes 签名 → 原样发送。
 * 单次序列化入口是 `buildBody()`（static，便于单测覆盖契约字段）。
 *
 * ## 能力降级
 *
 * 未编译 OpenSSL（WEAKNET_HAVE_TLS）或未链接 libcurl 时，start() 直接失败
 * 并写明原因，绝不"无签名裸发"。配置缺 device_id 或 url 时同样保持关闭。
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "process_net_profiler.hpp"
#include "site_incident.hpp"
#include "skb_drop_monitor.hpp"
#include "weaknet_config.hpp"
#include "wireless_event.hpp"

namespace weaknet_dbus {
class DatabaseManager;
}

namespace weaknet {

/// 单批最多携带的各类事实条数（与云端契约上限同源）。
/// 超过时截断到上限——宁可多轮发送，也不让单个签名报文无界增长。
constexpr size_t EDGE_WIRELESS_MAX_EVENTS_PER_BATCH = 1000;
constexpr size_t EDGE_WIRELESS_MAX_INCIDENTS_PER_BATCH = 200;
constexpr size_t EDGE_WIRELESS_MAX_BASELINES_PER_BATCH = 2000;

/// 上行器运行统计（日志/健康检查用）。
struct EdgeWirelessUplinkStats {
    uint64_t rounds{0};            ///< 完成的采集-发送轮次
    uint64_t events_sent{0};       ///< 已确认送达的事件条数
    uint64_t incidents_sent{0};    ///< 已确认送达的事故条数
    uint64_t baselines_sent{0};    ///< 已确认送达的基线画像条数
    uint64_t send_failures{0};     ///< HTTP 层失败次数
    uint64_t batches_sent{0};      ///< 成功送达的批次数
};

/**
 * @brief 一批待上行的无线事实（纯数据，便于单测直接构造）。
 *
 * `incident_evidence` 与 `incidents` **同序**：板端 site_incident_events 回链
 * 是证据可追溯性的载体，必须随事故本体一起上行，否则云端诊断无法枚举证据。
 */
struct WirelessUplinkPayload {
    std::string device_id;
    uint64_t watermark_ms = 0;   ///< 本批覆盖到的最大事实时间（仅作游标回显）

    std::vector<weaknet_dbus::WirelessDeviceEvent> events;
    std::vector<weaknet_dbus::SiteIncident> incidents;
    std::vector<std::vector<std::string>> incident_evidence;  ///< 与 incidents 同序
    std::vector<weaknet_dbus::DeviceLinkProfile> baselines;

    /// 深度内核快照（进程画像 Top N + 协议栈丢包归因）
    std::vector<weaknet_dbus::ProcessNetInfo> top_processes;
    std::optional<weaknet_dbus::DropStatsSummary> drop_stats;

    /// 环境窗口（Wi-Fi 侧证据与深度内核快照）。
    uint64_t env_from_ms = 0;
    uint64_t env_to_ms = 0;
};

/**
 * @brief 无线事实上行器。start() 起后台线程，stop() 幂等并 join。
 */
class EdgeWirelessUplinkExporter {
public:
    using ProfilerSampler = std::function<void(std::vector<weaknet_dbus::ProcessNetInfo>*)>;
    using DropSampler = std::function<void(weaknet_dbus::DropStatsSummary*)>;

    /**
     * @param config     运行时配置（读 edge.* 字段）
     * @param db         数据库管理器（事实的唯一来源）
     * @param state_path 游标文件路径（通常位于 data_dir 下）
     *
     * `profiler_sampler` / `drop_sampler` 是每次采集时才求值的回调，
     * 内部加锁保护，杜绝跨线程 UAF。
     */
    EdgeWirelessUplinkExporter(const weaknet_dbus::WeakNetConfig& config,
                               weaknet_dbus::DatabaseManager& db,
                               std::string state_path,
                               ProfilerSampler profiler_sampler = {},
                               DropSampler drop_sampler = {});
    ~EdgeWirelessUplinkExporter();

    EdgeWirelessUplinkExporter(const EdgeWirelessUplinkExporter&) = delete;
    EdgeWirelessUplinkExporter& operator=(const EdgeWirelessUplinkExporter&) = delete;

    /// 校验配置与能力并启动后台线程；false 表示未启动且无任何出站流量。
    bool start();
    /// 停止后台线程（幂等）。
    void stop();

    bool isRunning() const { return running_.load(); }
    EdgeWirelessUplinkStats stats() const;

    /// 当前游标（已确认送达的最大事实时间，毫秒）。
    uint64_t watermarkMs() const;

    /**
     * @brief 执行一轮：采集 → 序列化 → 签名 → 发送 → 推进游标。
     *
     * 公开以便单测在不启线程的前提下驱动完整流程（网络失败即返回 false
     * 且游标不前移）。生产路径由 run() 循环调用。
     */
    bool runOnce(std::string* error);

    /**
     * @brief 把一批事实序列化为 network.edge.wireless-events.v1 报文字节。
     *
     * 单次序列化入口：签名与发送都用这里产出的同一段字节。
     * 返回空串表示构造失败（error 填原因）。
     */
    static std::string buildBody(const WirelessUplinkPayload& payload, std::string* error);

    /// 事实时间字段过滤：只上行晚于游标的事实（严格大于，避免重复发送已确认项）。
    static WirelessUplinkPayload selectSince(const WirelessUplinkPayload& all,
                                             uint64_t watermark_ms);

    /// 读游标；文件缺失/损坏返回 0（宁可重发，绝不跳发）。
    /// 公开以便单测直接覆盖"损坏即重扫"的分支（与 EdgeTelemetryExporter
    /// 的 applyPendingActions 同一先例：为可测性开放，语义仍是内部实现）。
    uint64_t loadWatermark() const;

    /// 写游标；失败只记日志（下一轮重发，服务端幂等）。
    void storeWatermark(uint64_t value);

private:
    void run();

    /// 从 SQLite 采集一批事实（受各组的单批上限约束）。
    bool collect(WirelessUplinkPayload* out, std::string* error);

    std::string signBody(const std::string& body, std::string* error) const;
    bool postSigned(const std::string& url, const std::string& body,
                    const std::string& signature, std::string* response,
                    std::string* error) const;
    std::string uplinkUrl() const;
    bool configComplete(std::string* error) const;

    const weaknet_dbus::WeakNetConfig& config_;
    weaknet_dbus::DatabaseManager& db_;
    std::string state_path_;
    ProfilerSampler profiler_sampler_;
    DropSampler drop_sampler_;

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    mutable std::atomic<uint64_t> watermark_ms_{0};

    mutable std::mutex stats_mutex_;
    EdgeWirelessUplinkStats stats_;
};

}  // namespace weaknet
