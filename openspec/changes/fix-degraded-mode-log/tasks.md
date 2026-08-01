# Tasks: fix-degraded-mode-log

- [x] 1 添加降级模式日志
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `降级模式日志告警` | `降级模式触发 WARNING 日志`
  - Verify: `build`
  - 在 weak_netmgr.cpp 的 startTrafficAnalysis 中添加降级检查
