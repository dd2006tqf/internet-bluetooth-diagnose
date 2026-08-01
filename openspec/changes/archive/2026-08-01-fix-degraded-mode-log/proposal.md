# Proposal: fix-degraded-mode-log
## Why
startTrafficAnalysis 启动后不检查降级模式，外部无法感知。
## What
启动后检查 isDegradedMode()，若为 true 输出 LOG_WARNING。
