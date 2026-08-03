# WeakNet Server Spec — Delta for process-network-profiling

## ADDED Requirements
### Requirement: 进程网络画像
服务端 MUST 通过 eBPF 探针统计每个进程的网络流量，定位占带宽或大量重传的进程。

#### Scenario: eBPF进程统计探针
- **WHEN** 服务端启动且内核支持 eBPF
- **THEN** MUST 在现有 flow_rate.bpf.c 中新增 process_stats Map，按 PID 记录进程名、发送字节数、包数、重传次数

#### Scenario: 用户态监控接口
- **WHEN** ProcessNetProfiler 被调用 getTopBandwidth() 或 getTopRetransmit()
- **THEN** MUST 返回按带宽或重传排序的进程统计列表

### Requirement: 进程画像集成
进程级画像 MUST 集成到构建系统。

#### Scenario: 构建更新
- **WHEN** 服务端编译
- **THEN** MUST 编译新增的 process_net_profiler.cpp，server/Makefile 包含新的源文件
