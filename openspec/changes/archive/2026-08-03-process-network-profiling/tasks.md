# Tasks: process-network-profiling

- [x] 1 修改 flow_rate.bpf.c 增加进程统计 Map
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `进程网络画像` | `eBPF进程统计探针`
  - Verify: `build`
  - 修改 `server/src/flow_rate.bpf.c`，新增 process_stats Map，记录 pid/comm/发送字节/包数

- [x] 2 实现 ProcessNetProfiler 用户态读取
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `进程网络画像` | `用户态监控接口`
  - Verify: `build`
  - 实现 `process_net_profiler.hpp/.cpp`，从 BPF Map 读取进程级统计

- [x] 3 集成到 Makefile 和诊断入口
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `进程画像集成` | `构建更新`
  - Verify: `build`
  - 修改 server/Makefile 添加源文件，编译验证
