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
 * `data/wireless-uplink-state` 记录**三域独立游标**（events_ms /
 * incidents_ms / baselines_ms）："该域已确认送达的最大事实时间"。三域共用
 * 单水位曾造成跨域截断丢事实（事故/基线时间戳把水位推过截断在批上限之后
 * 的未发事件）——每个域只按**本域已发送行**的时间戳推进，谁也不替谁代言。
 * 游标只在 HTTP 2xx 之后前移；失败则原地重试——服务端按 id 幂等，
 * 因此重发是安全且常态的。单键损坏只回退该域（从 0 全量重扫）；旧格式
 * 文件缺失 incident/baseline 键时同样回退 0 重扫（单水位时代可能截断丢发
 * 的行靠重扫补回）。绝不猜测一个"更靠后"的位置而漏发。
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
#include "tcp_rtt_monitor.hpp"
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

/// 三域独立游标（毫秒）：每个域只按**本域已确认送达行**的时间戳推进。
/// 单水位会被事故/基线的更晚时间戳推过截断在批上限之后的未发事件（C2）。
struct WirelessUplinkCursors {
    uint64_t events_ms{0};
    uint64_t incidents_ms{0};
    uint64_t baselines_ms{0};
};

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
    uint64_t watermark_ms = 0;   ///< 采集时三域已确认游标的最大值（仅作游标回显，云端不依赖）

    std::vector<weaknet_dbus::WirelessDeviceEvent> events;
    std::vector<weaknet_dbus::SiteIncident> incidents;
    std::vector<std::vector<std::string>> incident_evidence;  ///< 与 incidents 同序
    std::vector<weaknet_dbus::DeviceLinkProfile> baselines;

    /// 深度内核快照（进程画像 Top N + 协议栈丢包归因 + 被动 TCP RTT 连接画像）
    std::vector<weaknet_dbus::ProcessNetInfo> top_processes;
    std::optional<weaknet_dbus::DropStatsSummary> drop_stats;
    std::optional<weaknet_dbus::TcpRttSummary> tcp_rtt_stats;

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
    using TcpRttSampler = std::function<void(weaknet_dbus::TcpRttSummary*)>;

    /**
     * @param config     运行时配置（读 edge.* 字段）
     * @param db         数据库管理器（事实的唯一来源）
     * @param state_path 游标文件路径（通常位于 data_dir 下）
     *
     * `profiler_sampler` / `drop_sampler` / `tcp_rtt_sampler` 是每次采集时才求值的回调，
     * 内部加锁保护，杜绝跨线程 UAF。
     */
    EdgeWirelessUplinkExporter(const weaknet_dbus::WeakNetConfig& config,
                               weaknet_dbus::DatabaseManager& db,
                               std::string state_path,
                               ProfilerSampler profiler_sampler = {},
                               DropSampler drop_sampler = {},
                               TcpRttSampler tcp_rtt_sampler = {});
    ~EdgeWirelessUplinkExporter();

    EdgeWirelessUplinkExporter(const EdgeWirelessUplinkExporter&) = delete;
    EdgeWirelessUplinkExporter& operator=(const EdgeWirelessUplinkExporter&) = delete;

    /// 校验配置与能力并启动后台线程；false 表示未启动且无任何出站流量。
    bool start();
    /// 停止后台线程（幂等）。
    void stop();

    bool isRunning() const { return running_.load(); }
    EdgeWirelessUplinkStats stats() const;

    /// 三域游标的最大值（健康回显；真实推进语义见 loadCursors）。
    uint64_t watermarkMs() const;

    /**
     * @brief 执行一轮：采集 → 序列化 → 签名 → 发送 → 三域游标各自推进。
     *
     * 公开以便单测在不启线程的前提下驱动完整流程（网络失败即返回 false
     * 且游标不前移）。生产路径由 run() 循环调用。
     */
    bool runOnce(std::string* error);

    /**
     * @brief 从 SQLite 采集一批事实（受各组的单批上限约束）。
     *
     * 各域严格按**本域游标**取数（事件 ts > events_ms、事故 last_event_ms
     * >= incidents_ms、基线 updated_at > baselines_ms），互不越界。
     * 公开以便单测直接覆盖三域游标的独立性（与 runOnce 同一可测性先例）。
     */
    bool collect(WirelessUplinkPayload* out, std::string* error);

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

    /**
     * @brief 送达成功后把已发送事实的时间戳**按域**回写到各自游标。
     *
     * 纯函数便于单测锁死 C2 语义：事故/基线更晚的时间戳绝不推进事件游标，
     * 否则截断在批上限之后的未发事件被永久跳过。任何域只增不减。
     */
    static WirelessUplinkCursors advanceCursors(const WirelessUplinkCursors& before,
                                                const WirelessUplinkPayload& sent);

    /// 读三域游标；文件缺失或单键损坏时该域回退 0（宁可重扫，绝不跳发）。
    /// 公开以便单测直接覆盖"缺失/旧格式/损坏即按域重扫"的分支（与
    /// EdgeTelemetryExporter 的 applyPendingActions 同一先例：为可测性开放）。
    WirelessUplinkCursors loadCursors() const;

    /// 写三域游标并同步内存游标；文件失败只记日志（下一轮重发，服务端幂等）。
    void storeCursors(const WirelessUplinkCursors& cursors);

private:
    void run();

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
    TcpRttSampler tcp_rtt_sampler_;

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    /// 三域运行期游标（collect 读、storeCursors 写；重启时由 loadCursors 恢复）
    mutable std::atomic<uint64_t> events_ms_{0};
    mutable std::atomic<uint64_t> incidents_ms_{0};
    mutable std::atomic<uint64_t> baselines_ms_{0};

    mutable std::mutex stats_mutex_;
    EdgeWirelessUplinkStats stats_;
};

}  // namespace weaknet
