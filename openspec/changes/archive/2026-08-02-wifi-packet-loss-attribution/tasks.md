# Tasks: wifi-packet-loss-attribution

- [x] 1 实现 Wi-Fi 收发丢包追踪 eBPF 程序
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `Wi-Fi丢包归因` | `eBPF收发丢包探针实现`
  - Verify: `build`
  - 实现 `server/src/wifi_packet_loss.bpf.c`，挂载收发路径 tracepoint，统计每接口收发/丢弃/重试包数

- [x] 2 实现 WifiPacketLossMonitor 用户态读取
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `Wi-Fi丢包归因` | `用户态监控接口`
  - Verify: `build`
  - 实现 `wifi_packet_loss_monitor.hpp/.cpp`，从 BPF Map 读取收发统计并计算丢包归因

- [x] 3 集成到网络质量评估和部署
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `系统集成` | `评估器集成和构建更新`
  - Verify: `build`
  - 修改 server/Makefile 添加编译规则，集成到 NetworkQualityAssessor 和事件推送
