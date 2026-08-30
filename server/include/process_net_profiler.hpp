/**
 * @file process_net_profiler.hpp
 * @brief 进程级网络画像监控器 — 用户态接口
 *
 * 从 eBPF Map（flow_rate.bpf.c 或独立程序）读取每个进程的网络流量统计，
 * 定位"哪个进程在占带宽"、"哪个进程在大量重传"。
 *
 * 挂点：kprobe/tcp_sendmsg（捕获 pid + comm）
 * Maps：process_stats（pid → 累计统计）
 *
 * 典型用途：
 *   - 用户报告"网络慢"时，快速定位流量大户
 *   - 识别异常重传进程（可能是恶意软件或故障服务）
 */

#pragma once

#include <string>
#include <vector>
#include <memory>
#include "ebpf_monitor_interface.hpp"
#include "ebpf_monitor_metrics.hpp"

namespace weaknet_dbus {

/// 单个进程的网络使用统计
struct ProcessNetInfo {
    uint32_t pid;              ///< 进程 ID
    std::string comm;          ///< 进程名（从 task_struct 读取，短名）
    uint64_t txBytes;          ///< 累计发送字节数
    uint64_t txPackets;        ///< 累计发送包数
    uint64_t retransCount;     ///< 累计重传次数
};

/**
 * @brief 进程级网络画像监控器
 *
 * 继承 IEbpfMonitor，统一健康/指标查询。
 * 使用 Pimpl 模式隐藏 libbpf 细节。
 */
class ProcessNetProfiler : public IEbpfMonitor {
public:
    ProcessNetProfiler();
    ~ProcessNetProfiler();

    bool init(const std::string& bpfObjPath);
    void stop();

    bool isInitialized() const { return initialized_; }
    bool isAvailable() const override { return available_; }

    const char* monitorName() const override { return "ProcessNetProfiler"; }
    EbpfMonitorState commonState() const override { return stateSupport_.state(); }
    EbpfMonitorHealth health() const override { return stateSupport_.health(); }
    EbpfMonitorMetrics metrics() const override { return stateSupport_.metrics(); }
    void resetMetrics() override { stateSupport_.resetMetrics(); }

    /// 获取所有进程的网络统计快照
    std::vector<ProcessNetInfo> getProcesses();

    /**
     * @brief 按发送字节数取 Top N（带宽大户）
     * @param topN 返回前 N 个
     */
    std::vector<ProcessNetInfo> getTopBandwidth(size_t topN);

    /**
     * @brief 按重传次数取 Top N（定位问题进程）
     * @param topN 返回前 N 个
     */
    std::vector<ProcessNetInfo> getTopRetransmit(size_t topN);

    /**
     * @brief 查询单个 pid
     * @param pid 进程 ID
     * @param out 输出参数
     * @return true 找到；false 无记录
     */
    bool getProcess(uint32_t pid, ProcessNetInfo* out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    bool initialized_ = false;
    bool available_ = false;
    EbpfMonitorStateSupport stateSupport_{"ProcessNetProfiler"};
};

}  // namespace weaknet_dbus
