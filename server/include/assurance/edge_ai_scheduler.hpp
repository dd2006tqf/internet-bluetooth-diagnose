#pragma once

/**
 * @file edge_ai_scheduler.hpp
 * @brief 端侧 AI 初筛异步调度器（T3 / Gate A 工业级隔离规范）
 *
 * ## 核心职责与铁律
 * 1. **有界任务队列（容量=1，并发=1）**：
 *    - 绝不并发拉起多个大模型推理进程；
 *    - 当等待槽已有尚未开始的任务时，新触发的异常直接覆盖等待槽（保留最新态势），
 *      但绝不打断正在唯一执行槽中运行的推理进程。
 * 2. **冷却键去重（Cooldown = 300s）**：
 *    - 冷却键定义：`device_id + ":" + interface + ":" + anomaly_type`；
 *    - 同一冷却键在 300 秒内只允许入队一次，杜绝高频劣化引发推理风暴。
 * 3. **15 秒真正硬超时清理（Hard Kill）**：
 *    - 通过 `fork() + exec()` 启动独立进程组（`setpgid(0, 0)`）；
 *    - 父线程通过 `waitpid(pid, &status, WNOHANG)` 以 100ms 步进轮询；
 *    - 满 15 秒未退出或服务请求停止时，立即发送 `kill(-pid, SIGKILL)` 强杀整个进程组并回收，
 *      彻底杜绝僵尸进程或后台挂起占满 CPU。
 * 4. **主流程零阻塞**：
 *    - `submitIfEligible()` 为 O(1) 内存操作，微秒级返回，对 eBPF 采样、SLE 评估和遥测上报零阻塞。
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

#include "assessment_snapshot.hpp"
#include "assurance/edge_telemetry_serializer.hpp"

namespace weaknet {

struct EdgeAiSchedulerStats {
    uint64_t submitted_total{0};
    uint64_t skipped_healthy{0};
    uint64_t skipped_cooldown{0};
    uint64_t overwritten_pending{0};
    uint64_t completed_ok{0};
    uint64_t failed_or_killed{0};
    uint64_t timed_out_killed{0};
};

class EdgeAiScheduler {
public:
    using HypothesisSink = std::function<void(EdgeAiHypothesisRecord)>;

    explicit EdgeAiScheduler(
        std::string device_id,
        HypothesisSink sink,
        std::chrono::seconds cooldown = std::chrono::seconds(300),
        std::chrono::milliseconds hard_timeout = std::chrono::milliseconds(15000),
        std::string script_path = "/home/radxa/weaknet/edge_network_rag.py");

    ~EdgeAiScheduler();

    EdgeAiScheduler(const EdgeAiScheduler&) = delete;
    EdgeAiScheduler& operator=(const EdgeAiScheduler&) = delete;

    void start();
    void stop();
    bool isRunning() const { return running_.load(); }

    /**
     * @brief 非阻塞提交评估快照。仅当处于 DEGRADED/BAD 且未在冷却期内时入队。
     * @return true 表示成功放入有界等待槽；false 表示健康或被冷却拦截。
     */
    bool submitIfEligible(std::shared_ptr<const AssessmentSnapshot> snapshot);

    EdgeAiSchedulerStats stats() const;

    /// 供单元测试注入自定义命令行或脚本路径
    void setCustomCommandForTest(std::string cmd) {
        std::lock_guard<std::mutex> lock(mutex_);
        custom_cmd_for_test_ = std::move(cmd);
    }

private:
    void workerLoop();
    bool executeWithHardTimeout(const AssessmentSnapshot& snap,
                                EdgeAiHypothesisRecord* out_record);

    std::string device_id_;
    HypothesisSink sink_;
    std::chrono::seconds cooldown_;
    std::chrono::milliseconds hard_timeout_;
    std::string script_path_;
    std::string custom_cmd_for_test_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;

    /// 容量为 1 的有界等待槽（新任务覆盖旧等待任务）
    std::shared_ptr<const AssessmentSnapshot> pending_slot_;

    /// 冷却表：key = device_id:interface:anomaly_type -> 上次允许触发的 steady_clock 时间
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> cooldown_map_;

    EdgeAiSchedulerStats stats_;

    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
};

}  // namespace weaknet
