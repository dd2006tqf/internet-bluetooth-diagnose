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
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "assessment_snapshot.hpp"
#include "assurance/edge_telemetry_serializer.hpp"
#include "weaknet_config.hpp"

namespace weaknet_dbus {
class ConfigTransaction;  // weaknet_config.hpp
}

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
    uint64_t unsigned_responses_rejected{0};  ///< S1：未签名/验签失败的响应整批拒绝数
    uint64_t action_results_sent{0};  ///< 已回传服务端并获确认的动作结果数
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

/// 一条已就地执行、待回传服务端确认的下行动作结果。
///
/// 与遥测记录一样，结果在产生时刻即完成序列化与签名——签名覆盖的是
/// action-results 报文的最终字节，回传时原样发送，绝不重新序列化。
struct EdgeActionResultRecord {
    std::string action_id;
    std::string status;     ///< "APPLIED" | "REJECTED"
    std::string detail;
    std::string body;       ///< network.edge.action-results.v1 报文字节
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
    /**
     * @param config       运行时配置（引用 ServerContext::cfg，事务内由
     *                     ConfigTransaction 协调写入）
     * @param hostname     设备名，写入 display_name
     * @param config_txn   云端下发的三态事务；nullptr 时退回纯 setMonitorParam
     *                     行为（与现状一致，便于回退与单元测试）
     */
    EdgeTelemetryExporter(const weaknet_dbus::WeakNetConfig& config,
                          std::string hostname,
                          std::shared_ptr<weaknet_dbus::ConfigTransaction> config_txn = nullptr);
    ~EdgeTelemetryExporter();

    /// 全局 libcurl 初始化与清理（供服务主入口在单线程上下文显式调用）
    static void initGlobal();
    static void cleanupGlobal();

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
     * @param snapshot 以 shared_ptr 传入并**由 exporter 共同持有**。
     *        这一点是正确性要求，不是风格选择：exporter 会把该指针暂存
     *        供后续看门狗（evaluateTrialDeadline）使用，而调用方持有的
     *        shared_ptr 在调用返回后可能被释放——若此处只收 const& 再以
     *        空 deleter 包装，暂存的就是裸悬垂指针，TRIAL 到期时读已释放
     *        内存。传 shared_ptr 使 exporter 成为共同所有者，从根上消除
     *        该悬垂。
     *
     * @return true 表示已入队（含挤掉最旧项的情形）；false 表示未启用。
     */
    bool enqueue(std::shared_ptr<const AssessmentSnapshot> snapshot);

    /// 当前统计快照（线程安全，用于日志/健康检查）。
    EdgeExporterStats stats() const;

    /// 是否已成功启动（配置完整且线程在跑）。
    bool isRunning() const { return running_.load(); }

    /// 执行服务端下发的动作（走既有白名单校验），并把结果暂存待回传。
    /// S1：配置了 edge.cloud_public_key_path 时，响应中的 pending_actions
    /// 必须携带对该数组字节的有效 Ed25519 detached 签名，否则整批拒绝
    /// （unsigned_responses_rejected++），单条动作一律不落地。
    /// 设为 public 方便单测与调试。
    void applyPendingActions(const std::string& response_body);

    /// 消费暂存的 OTA 升级回执并（仅在有终态时）排队上报。回执生命周期两段式：
    /// schedule 时只写 action 上下文（result="IN_PROGRESS"），终态必须由升级
    /// 脚本自身写入（成功转正后 "SUCCEEDED"、失败/回滚 "FAILED"）；本方法只
    /// 如实上报脚本写下的终态，绝不凭"重启发生了"推断升级成功。IN_PROGRESS
    /// 超时（升级进程死亡）则如实上报 FAILED。设为 public 方便单测与调试。
    void checkAndEmitPendingOtaReceipt();

    /// 把一条 ROLLBACK 回执入队（由 forceRollback 触发后调用方显式发送）。
    void emitRollbackReceipt(const std::string& reason);

    /**
     * @brief 把一次**本地**配置变更作为 L1 事实回执入队。
     *
     * 与 applyPendingActions 的区别：那些动作由云端下发、云端持有
     * network_pending_actions 记录，回执只需带 action_id/claim_token；本地
     * 调参（D-Bus SetMonitorParam，即 weaknet-cli set）完全不经云端队列，
     * 云端没有任何记录可关联，因此回执必须自带 config_key/config_value
     * 才能被描述成一行 L1 outcome（execution_origin=LOCAL_OPERATION）。
     *
     * action_id 取 `nlocal-` 前缀，与云端队列的 id 空间显式分开。
     * 每次调用生成全新 id：同一 key 后续再改是**另一次**变更，不是重发。
     *
     * 仅在变更**已成功写入**后调用——未生效的本地写不改变设备状态，不构成事实。
     *
     * @param config_generation 变更后的 config_generation（弱网下云端只能从
     *        快照读到代次，本字段让它能与具体 key/value 对齐）。
     */
    void emitLocalConfigChange(const std::string& key, const std::string& value,
                               const std::string& detail, uint32_t config_generation);

    /**
     * @brief 在 TRIAL 到期时根据最新快照决定 commit 或 rollback。
     *
     * 由 exporter 的 run() 主循环调用；传入的 latest_snapshot 是当前
     * ServerContext::assessment_store 的最新不可变快照（nullptr 表示无可用
     * 快照，按 fail-safe 处理 = ROLLBACK）。
     *
     * 返回 true 表示发生了一次状态迁移（commit 或 rollback）。
     */
    bool evaluateTrialDeadline(const AssessmentSnapshot* latest_snapshot);

    /**
     * @brief 注入最新快照指针（由 quality 线程在 publish 之后调用）。
     *
     * exporter 在 TRIAL 到期时据此判定健康，避免反向依赖 ServerContext。
     * 线程安全：内部以 mutex + shared_ptr 持有。
     */
    void injectLatestSnapshot(std::shared_ptr<const AssessmentSnapshot> snap);

    /// 看门狗心跳。若当前在 TRIAL 且 deadline 已过：
    /// 返回 true 表示需要调用方处理（deadline 已过且仍在 TRIAL）。
    bool tickWatchdog(std::chrono::steady_clock::time_point now) const;

    /// 当前等待回传的动作结果条数。测试用来确认一条本机回执确实被序列化
    /// 并入队；生产路径不做观察性消费（统计请走 stats()）。
    size_t pendingActionResultCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return pending_action_results_.size();
    }

    /// 取待回传动作结果的副本（含 action_id/status/detail）。测试用来断言回执
    /// 终态语义（如 OTA 的 APPLIED/REJECTED 区分）；生产路径不做观察性消费
    /// （统计请走 stats()）。
    std::vector<EdgeActionResultRecord> pendingActionResultsSnapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return {pending_action_results_.begin(), pending_action_results_.end()};
    }

    /**
     * @brief 入队一条端侧 AI 初筛假设（L2 EdgeAIHypothesis），将在匹配对应快照时随信封一同签名上报。
     */
    void enqueueHypothesis(EdgeAiHypothesisRecord hypothesis);

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

    /// 序列化并签名一条动作结果，暂存到待回传队列。
    /// status 为服务端契约终态："APPLIED" | "REJECTED" | "ROLLBACK"。
    /// claim_token/generation 是 v2 契约字段，v1 设备可填空。
    /// config_key/config_value 仅在**本地**变更（云端无 pending_action 记录）
    /// 时传入并写进报文；云端下发的动作由服务端从队列表回读，此处保持空串
    /// 以免两处来源打架。
    void queueActionResult(const std::string& action_id, const char* status,
                           const std::string& detail,
                           const std::string& claim_token = "",
                           uint64_t generation = 0,
                           const std::string& config_key = "",
                           const std::string& config_value = "");

    /// 把待回传的动作结果上报到 /edge/action-results；返回是否全部送达。
    bool transmitActionResults(std::string* error);

    /// 由 telemetry URL 派生 action-results URL（同主机同前缀，换尾段）。
    std::string actionResultsUrl() const;

    /// 调度后台异步 OTA 升级（fork+exec 独立守护进程执行 weaknet-local-update.sh apply）。
    /// 成功时把 action 上下文落盘（result="IN_PROGRESS"），fork 失败则立即回滚该文件。
    /// 升级脚本通过环境变量 WEAKNET_OTA_RECEIPT 拿到该路径，成功转正后写
    /// "SUCCEEDED"、失败（EXIT 非零）写 "FAILED"；已有 IN_PROGRESS 回执时拒绝新指令。
    bool scheduleOtaUpgrade(const std::string& action_id, const std::string& claim_token,
                            uint64_t generation, const std::string& url, std::string* error);

    /// 发送单个已签名 body 到指定 URL，返回 HTTP 状态码与响应体。
    bool postSigned(const std::string& url, const std::string& body,
                    const std::string& signature, std::string* response,
                    std::string* error);

    bool configComplete(std::string* error) const;

    const weaknet_dbus::WeakNetConfig& config_;
    std::string hostname_;
    std::shared_ptr<weaknet_dbus::ConfigTransaction> config_txn_;

    /// 最近一次 enqueue 的快照（TRIAL 健康评估的输入）。
    /// 由 injectLatestSnapshot 更新，由 evaluateTrialDeadline 读取。
    mutable std::mutex latest_snapshot_mutex_;
    std::shared_ptr<const AssessmentSnapshot> latest_snapshot_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<EdgeTelemetryRecord> buffer_;
    ///< 已就地执行、待回传服务端确认的动作结果（保留至确认送达）。
    std::deque<EdgeActionResultRecord> pending_action_results_;
    ///< 待随遥测信封上报的端侧 AI 初筛假设（有界缓冲，最多保留 8 条）。
    std::deque<EdgeAiHypothesisRecord> pending_hypotheses_;

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};

    /// 已执行过的云端 action_id → 首次执行结果（成功/失败）。
    /// 去重表存结果而不仅是存在性：首次被拒的动作在回执丢失后被云端重放时，
    /// 必须回执真实结果而不是谎报 APPLIED。
    /// 进程内即可：重启后服务端会把 action 重新标记为 DELIVERED 并等待回执，
    /// 此时重建执行是正确行为。
    std::unordered_map<std::string, bool> executed_action_ids_;

    /// S1：云端下行验签。edge.cloud_public_key_path 非空时按需加载 Ed25519
    /// 公钥（void* 保存 EVP_PKEY*，不把 OpenSSL 头暴露进本头文件）；
    /// 加载失败保持启用且指针为空 = 全部响应拒绝（fail-closed）。
    void* cloud_public_key_{nullptr};
    bool downlink_verify_enabled_{false};
    std::once_flag downlink_key_once_;
    void ensureDownlinkKeyLoaded();
    /// 对响应内 pending_actions 数组原始字节做 Ed25519 验签。
    bool verifyPendingActionsSignature(const std::string& body) const;

    mutable std::mutex stats_mutex_;
    EdgeExporterStats stats_;
};

}  // namespace weaknet
