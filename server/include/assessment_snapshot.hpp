#pragma once

/**
 * @file assessment_snapshot.hpp
 * @brief 权威评估快照 — 单一事实源（W2）
 *
 * ## 为什么需要
 *
 * 实测存在三条独立评估路径，各自拉不同 metrics、各自调 OverallPolicy，
 * 结论可能不一致：
 *   1. quality 线程（server.cpp，传全部 SLE）→ 信号
 *   2. history 持久化线程（曾只传 5 个 Core SLE）→ DB
 *   3. HealthCheck（dbus_service.cpp）→ toHealthCheckJson
 *
 * 修正：quality 线程是**唯一 evaluator 执行点**；每轮评估 + stabilizer
 * 更新后发布不可变 snapshot；其余消费者只读，绝不重新拉 metrics、
 * 重新 evaluate、重新调 OverallPolicy。
 *
 * ## 生命周期边界（三种，缺一即语义漏洞）
 *
 *   1. daemon 启动后尚无第一份 snapshot → 消费者得显式 UNKNOWN(no_assessment_yet)
 *   2. Profile/配置变更 → config_generation 不符 → 旧 snapshot 失效
 *      （否则可能出现"配置已切到 INTERNET_ACCESS，D-Bus 还返回旧 profile 的 GOOD"）
 *   3. 网络重连/epoch 变化 → network_epoch 不符 → 旧 snapshot 失效
 *      （Stabilizer 随 epoch reset 是既有 SR-4 语义）
 *
 * ## history 同代冻结
 *
 * history 写入的 assessment **和**原始 metrics 都从同一个 snapshot 取——
 * 不能 assessment 用 t0 的 snapshot 而 metrics 从 registry 取 t0+2s，
 * 否则"结论不一致"只是变成"结论和证据时间不一致"。
 *
 * ## 实现约束
 *
 * 项目为 C++17，不为此升级语言标准：用 std::mutex + std::shared_ptr
 * （std::atomic_load/atomic_store 自由函数），不使用 C++20 的
 * std::atomic<std::shared_ptr<T>>。
 */

#include "assurance/network_experience.hpp"
#include "assurance/health_state.hpp"
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <cstdint>

namespace weaknet {

/// 不可变的权威评估快照（发布后不得修改）
struct AssessmentSnapshot {
    uint64_t sequence_id{0};                  ///< 单调递增发布序号
    std::chrono::steady_clock::time_point evaluated_at_monotonic{}; ///< 评估时刻（单调钟）
    std::chrono::system_clock::time_point wall_timestamp{};         ///< 评估时刻（墙钟，日志/DB 用）
    AssessmentProfile profile{AssessmentProfile::INTERNET_ACCESS};  ///< 快照当时的 profile
    uint32_t config_generation{0};            ///< 配置代（变更即失效）
    uint64_t network_epoch{0};                ///< 网络代（重连/epoch 变化即失效）
    NetworkExperience experience;             ///< stabilizer 后的最终结论 + 全部 SLE + source/scope
};

/**
 * @brief 快照仓库：单写者（quality 线程）多读者
 *
 * 写侧：publish() 由 quality 线程每轮评估后调用。
 * 读侧：latest() 返回当前快照的 shared_ptr（不可变），或 nullptr 表示
 *       "尚无第一份评估"——消费者必须显式处理该情形，不得当成 GOOD。
 */
class AssessmentSnapshotStore {
public:
    /// 发布新快照（quality 线程独占调用）
    void publish(std::shared_ptr<const AssessmentSnapshot> snap) {
        std::lock_guard<std::mutex> lock(mutex_);
        current_ = std::move(snap);
    }

    /**
     * @brief 读取当前快照
     * @return 可能返回 nullptr —— 表示 daemon 启动后尚无第一份评估。
     *         调用方必须转成显式 UNKNOWN(no_assessment_yet)，不得返回空对象。
     */
    std::shared_ptr<const AssessmentSnapshot> latest() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return current_;
    }

    /// 供消费者判断 snapshot 是否仍然有效（配置代/网络代未变）
    static bool isCurrent(const AssessmentSnapshot& snap,
                          uint32_t config_generation, uint64_t network_epoch) {
        return snap.config_generation == config_generation &&
               snap.network_epoch == network_epoch;
    }

private:
    mutable std::mutex mutex_;
    std::shared_ptr<const AssessmentSnapshot> current_;
};

}  // namespace weaknet
