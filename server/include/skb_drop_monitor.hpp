/**
 * @file skb_drop_monitor.hpp
 * @brief 内核协议栈 Socket / skb 丢包原因精确归因监控器 — 用户态接口
 *
 * 通过挂载 tracepoint/skb/kfree_skb，读取内核协议栈丢弃数据包时的详细原因枚举（enum skb_drop_reason）。
 * 帮助区分网络故障根因：防火墙拦截（NETFILTER_DROP）、目标端口未监听（NO_SOCKET）、
 * 校验和损坏（TCP_CSUM/UDP_CSUM/IP_CSUM）、反向路由过滤（IP_RPFILTER）等。
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include "ebpf_monitor_interface.hpp"
#include "ebpf_monitor_metrics.hpp"

namespace weaknet_dbus {

/// 单项丢包原因聚合统计
struct DropReasonItem {
    uint32_t reasonCode = 0;       ///< 内核 enum skb_drop_reason 数值
    std::string reasonName;        ///< 内核枚举字符串（如 "NETFILTER_DROP"）
    std::string humanDesc;         ///< 友好的人类可读中文解释
    std::string protocol;          ///< 协议（"TCP", "UDP", "IPv4", "IPv6" 等）
    uint64_t count = 0;            ///< 累计丢包次数
    uint64_t lastTimestampNs = 0;  ///< 最近一次丢包纳秒时间戳
};

/// 丢包归因全局统计快照
struct DropStatsSummary {
    uint64_t totalDrops = 0;                ///< 累计捕获的总丢包数
    std::vector<DropReasonItem> topReasons; ///< 按丢包次数降序排列的高频丢包原因
};

/// 原因码到可读名称及解释的辅助转换函数
const char* skbDropReasonToString(uint32_t reason);
const char* skbDropReasonToDescription(uint32_t reason);

class SkbDropMonitor : public IEbpfMonitor {
public:
    SkbDropMonitor();
    ~SkbDropMonitor() override;

    /**
     * @brief 初始化并加载 BPF 程序
     * @param bpfObjPath BPF 目标文件路径（通常为 build/skb_drop.bpf.o）
     * @return true 成功挂载；false 降级或加载失败
     */
    bool init(const std::string& bpfObjPath = "build/skb_drop.bpf.o");

    /**
     * @brief 停止监控并释放 BPF 探针资源
     */
    void stop();

    /**
     * @brief 获取丢包原因统计汇总
     */
    DropStatsSummary getDropStats();

    // ---- IEbpfMonitor 接口实现 ----
    const char* monitorName() const override { return "SkbDropMonitor"; }
    EbpfMonitorState commonState() const override;
    bool isAvailable() const override;
    EbpfMonitorHealth health() const override;
    EbpfMonitorMetrics metrics() const override;
    void resetMetrics() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace weaknet_dbus
