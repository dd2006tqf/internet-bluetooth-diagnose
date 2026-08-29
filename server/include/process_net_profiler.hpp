// process_net_profiler.hpp
// 进程级网络画像 - 用户态接口
// 从 BPF Map 读取每个进程的网络流量统计，定位占带宽或大量重传的进程

#pragma once

#include <string>
#include <vector>
#include <memory>
#include "ebpf_monitor_interface.hpp"
#include "ebpf_monitor_metrics.hpp"

namespace weaknet_dbus {

// 进程网络统计
struct ProcessNetInfo {
    uint32_t pid;
    std::string comm;
    uint64_t txBytes;
    uint64_t txPackets;
    uint64_t retransCount;
};

// 进程级网络画像监控器
class ProcessNetProfiler : public IEbpfMonitor {
public:
    ProcessNetProfiler();
    ~ProcessNetProfiler();

    // 初始化（加载 BPF 对象）
    bool init(const std::string& bpfObjPath);

    // 停止并清理
    void stop();

    // 是否已初始化
    bool isInitialized() const { return initialized_; }

    // 是否可用（BPF 加载成功）
    bool isAvailable() const override { return available_; }

    const char* monitorName() const override { return "ProcessNetProfiler"; }
    EbpfMonitorState commonState() const override { return stateSupport_.state(); }
    EbpfMonitorHealth health() const override { return stateSupport_.health(); }
    EbpfMonitorMetrics metrics() const override { return stateSupport_.metrics(); }
    void resetMetrics() override { stateSupport_.resetMetrics(); }

    // 获取所有进程的网络统计
    std::vector<ProcessNetInfo> getProcesses();

    // 按发送字节数取 Top N
    std::vector<ProcessNetInfo> getTopBandwidth(size_t topN);

    // 按重传次数取 Top N（定位问题进程）
    std::vector<ProcessNetInfo> getTopRetransmit(size_t topN);

    // 查询单个 pid
    bool getProcess(uint32_t pid, ProcessNetInfo* out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    bool initialized_ = false;
    bool available_ = false;
    EbpfMonitorStateSupport stateSupport_{"ProcessNetProfiler"};
};

}  // namespace weaknet_dbus