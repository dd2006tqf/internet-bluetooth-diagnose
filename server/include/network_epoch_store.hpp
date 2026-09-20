#pragma once

/**
 * @file network_epoch_store.hpp
 * @brief 网络代次（network epoch）的跨重启持久化
 *
 * ## 为什么需要它
 *
 * 中心平台按 ``(tenant, device, network_epoch, sequence_id)`` 对上行遥测
 * 幂等入库。板端 ``sequence_id`` 是**进程内**计数器（``assessment_sequence``），
 * 每次重启都从 1 重新开始；若 ``network_epoch`` 也随重启回到 1，则重启后发出的
 * ``(1, 1)``、``(1, 2)``… 会与上一轮运行留下的历史键完全重合。
 *
 * 后果不是报错，而是**静默丢弃**：服务端判定重复，返回 HTTP 200，
 * 板端日志里连一条失败都没有，但库里不再有新数据。这种故障从任何单侧
 * 日志都看不出来，只能靠"代次必须跨重启推进"这一不变式来排除。
 *
 * ## 语义
 *
 * - 首次运行（无状态文件）：返回 1。与既有部署的历史数据保持一致，
 *   不制造一次性的键空间跳变。
 * - 每次 open()：读取上次的值并 +1，立即写回。因此一次进程生命周期
 *   恰好消耗一个代次。
 * - 状态文件缺失、损坏或含非法值：**不回落**到可能已被用过的值
 *   （尤其不能回落 1），而是从高位重新开始。代次跳变的代价只是运维
 *   视图里多一个代次，而复用旧代次会导致数据丢失。
 * - 无法写盘（目录不存在等）：仍返回可用代次并在内存中自增，
 *   不因持久化失败而阻断上报。
 *
 * ## 与 DNS binding epoch 的关系
 *
 * 运行时另有 ``dns_binding_epoch``（路由/解析器变化时推进，见
 * ``DnsTransactionTracker::advanceBindingEpoch``）。本类是它的**下界**：
 * 启动时把 epoch 初始化到持久化值，运行中 DNS 变化仍可继续推进。
 * 两者语义一致——都是"观测代次"，区别只在推进的触发条件。
 */

#include <cstdint>
#include <string>

namespace weaknet {

/**
 * @brief 持久化网络代次计数器。
 *
 * 非线程安全：设计上只在启动路径单线程使用一次，运行中的推进由
 * ``DnsTransactionTracker`` / ``ServerContext::dns_binding_epoch`` 承担。
 */
class NetworkEpochStore {
public:
    /// 状态损坏或非法时的重新起算点。取一个远高于实际使用的值，
    /// 使"是否复用历史代次"这个问题答案为否。
    static constexpr uint64_t kRecoveryEpoch = 1000000;

    /**
     * @param state_path 状态文件完整路径（通常位于 data_dir 下）
     */
    explicit NetworkEpochStore(std::string state_path);

    /**
     * @brief 读取上次代次、+1、写回，返回本次应使用的代次。
     *
     * 可重复调用；同一实例第二次调用会再次 +1。
     */
    uint64_t open();

    /// 最近一次 open() 返回的代次；未 open 时为 0。
    uint64_t current() const { return current_; }

    /// 最近一次 open() 中的磁盘写回是否成功。写失败时已在内存中恢复代次并继续运行。
    bool isPersisted() const { return persisted_; }

private:
    /// 读取状态文件；不存在或内容非法时返回 false。
    bool readState(uint64_t* out) const;

    /// 原子写入状态文件（先写临时文件再 rename，避免掉电写坏）。
    bool writeState(uint64_t value) const;

    std::string state_path_;
    uint64_t current_{0};
    bool persisted_{false};
};

}  // namespace weaknet
