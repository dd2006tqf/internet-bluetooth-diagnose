# Proposal: fix-degraded-mode-log

## Why
`startTrafficAnalysis` 启动后不检查 TrafficAnalyzer 是否进入降级模式，外部调用者无法感知降级状态。

## What
启动流量分析后检查 `isDegradedMode()`，如果为 true 则输出 WARNING 日志。
