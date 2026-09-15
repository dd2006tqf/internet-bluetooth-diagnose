#pragma once

/**
 * @file edge_telemetry_exporter.hpp
 * @brief 边缘遥测上报器 — 把不可变评估快照推送到中心平台
 *
 * ## 为什么在 weaknet-dbus-server 进程内，而不是独立进程
 *
 * `http_latency.bpf.c` / `tcp_retransmit.bpf.c` 按 **PID** 过滤自流量。
 * 上报器与主服务同 PID，其产生的 TCP 连接天然落在既有自流量排除范围内，
 * 不会把自己伪装成"业务流量"计入被动指标。独立进程则需要把新 PID 再
 * 登记进各个 BPF Map，任何遗漏都会污染被观测数据。
 *
 * ## 断网语义（环形缓冲）
 *
 * 弱网环境下"上报失败"是常态而非异常。失败时快照保留在**容量固定的
 * 环形缓冲**里（覆盖最旧），网络恢复后按序批量补发。服务端按
 * (tenant, device, network_epoch, sequence_id) 幂等入库，因此：
 *   - 重复补发 → 服务端丢弃重复项，不产生重复历史；
 *   - 响应丢失导致的重发 → 同上，安全。
 *
 * 缓冲容量固定是刻意的：无界队列会让长时间断网直接吃光内存，而这块板子
 * 同时还在跑 eBPF 与评估线程。
 *
 * ## 签名与序列化顺序（关键约束）
 *
 * 请求体是"签名所覆盖的原始字节"。实现必须：
 *   1. 先把 body 序列化成一段 bytes；
 *   2. 对**这段 bytes** 做 Ed25519 签名；
 *   3. 原样发送这段 bytes，**不得**再序列化一次。
 *
 * 跨语言（C++ 生产 / Python 校验）下，任何"各自规范化再签名"的方案都会
 * 因数字格式、Unicode 转义、键序差异而静默失配，因此本实现从设计上就不
 * 给这种失配留下空间。
 *
 * ## 能力降级
 *
 * 未编译进 OpenSSL（WEAKNET_HAVE_TLS 未定义）时，本类仍可编译与实例化，
 * 但 start() 会直接失败并给出明确日志，绝不"无签名裸发"。
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "assessment_snapshot.hpp"
#include "weaknet_config.hpp"

namespace weaknet {

/// 环形缓冲中保留的待补发快照数上限。
///
/// 与上报周期（默认 10s）相配：约覆盖 100 秒的网络中断。取 10 是权衡结果——
/// 太小则短暂断连即丢证据，太大则长时间断网下内存与补发流量都不划算。
constexpr size_t EDGE_TELEMETRY_BUFFER_CAPACITY = 10;

/// 上报器运行状态，供日志与健康检查使用。
struct EdgeExporterStats {
    uint64_t snapshots_enqueued{0};   ///< 进入缓冲的快照总数
    uint64_t snapshots_sent{0};       ///< 成功送达的快照数（含批量补发）
    uint64_t duplicates_rejected{0};  ///< 服务端判定为重复的数量
    uint64_t send_failures{0};        ///< HTTP 层失败次数
    uint64_t dropped_oldest{0};       ///< 因缓冲满而挤掉的旧快照数
    uint64_t actions_applied{0};      ///< 成功落地的下行动作数
    uint64_t actions_rejected{0};     ///< 被白名单/范围校验拒绝的动作数
    size_t buffered{0};               ///< 当前缓冲深度
};

/**
 * @brief 一条待上报的评估快照及其派生的上报字段。
 *
 * 在 enqueue 时刻即完成序列化与签名，而不是在发送线程里做。
 * 原因：快照本体（experience 内的 SLE 证据）不在发送时重新读取，
 * 避免"结论用的是 t0 的快照、证据却读了 t1"的错配。
 */
struct EdgeTelemetryRecord {
    uint64_t sequence_id{0};
    uint64_t network_epoch{0};
    uint32_t config_generation{0};
    int64_t wall_timestamp_ms{0};
    std::string body;       ///< 已序列化、且已被签名覆盖的 body 字节
    std::string signature;  ///< body 的 Ed25519 签名（hex）
};

/**
 * @brief 边缘遥测上报器。
 *
 * 生命周期：start() 后由后台线程独占消费缓冲；stop() 幂等，
 * 并保证线程 join 完成（与 ServerContext 的其它线程同构）。
 */
class EdgeTelemetryExporter {
public:
    EdgeTelemetryExporter(const weaknet_dbus::WeakNetConfig& config,
                          std::string hostname);
    ~EdgeTelemetryExporter();

    EdgeTelemetryExporter(const EdgeTelemetryExporter&) = delete;
    EdgeTelemetryExporter& operator=(const EdgeTelemetryExporter&) = delete;

    /**
     * @brief 校验配置并启动后台线程。
     * @return false 表示配置不完整或能力缺失；此时线程未启动，
     *         且不得有任何出站流量。
     */
    bool start();

    /// 停止后台线程；幂等，可在析构前显式调用。
    void stop();

    /**
     * @brief 把一份快照放入待发送缓冲（非阻塞，O(1)）。
     *
     * 由 quality 线程在 publish 之后调用。此方法**不做**任何网络 I/O，
     * 也**不抛异常**：上报绝不能拖慢或中断评估主循环。
     *
     * @return true 表示已入队（含挤掉最旧项的情形）；false 表示未启用。
     */
    bool enqueue(const AssessmentSnapshot& snapshot);

    /// 当前统计快照（线程安全，用于日志/健康检查）。
    EdgeExporterStats stats() const;

    /// 是否已成功启动（配置完整且线程在跑）。
    bool isRunning() const { return running_.load(); }

private:
    void run();

    /// 取走缓冲中全部记录（保持入队顺序）。批量补发是断网恢复后的常态路径。
    std::vector<EdgeTelemetryRecord> drain();

    /// 构造并签名一条记录；失败返回 std::nullopt 语义（空 body）。
    bool buildRecord(const AssessmentSnapshot& snapshot, EdgeTelemetryRecord* out,
                     std::string* error);

    /// 对给定字节做 Ed25519 签名，返回 hex；失败返回空串并填 error。
    std::string signBody(const std::string& body, std::string* error);

    /// POST 一批记录；成功时解析响应中的 pending_actions 并就地执行。
    bool transmit(const std::vector<EdgeTelemetryRecord>& records, std::string* error);

    /// 执行服务端下发的动作（走既有白名单校验），并把结果暂存待回传。
    void applyPendingActions(const std::string& response_body);

    bool configComplete(std::string* error) const;

    const weaknet_dbus::WeakNetConfig& config_;
    std::string hostname_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<EdgeTelemetryRecord> buffer_;
    std::deque<std::string> pending_action_results_;  ///< 待随下次上报回传

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};

    mutable std::mutex stats_mutex_;
    EdgeExporterStats stats_;
};

}  // namespace weaknet
