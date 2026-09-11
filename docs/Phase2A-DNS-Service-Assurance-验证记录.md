# WeakNet Phase 2A: DNS Service Assurance — 实施与验证记录

> 日期：2026-09-11
> 分支：dev/feature（工作流外实施，经用户确认不走 OpenSpec change）
> 前置：Phase 1 Network Assurance Engine（State/Evidence-first）已封板

## 一、交付内容

### 1. 数据模型与核心引擎（server/include/assurance/）
- `dns_types.hpp`：DnsCanonicalKey（SR-7）、FingerprintQuality（ENRICHED/PARTIAL）、DnsTransactionState（10 态，TC/RCODE 互斥）、DnsMetricWindow（含 failure_ratio/ambiguity_ratio/insert_failure_ratio/event_delivery_loss_ratio/classification_coverage 数学语义）、DnsTrackerSnapshot（修正 #3）、AssessmentProfile（IR-3）
- `dns_transaction_tracker.hpp/.cpp`：userspace 唯一 lifecycle authority（SR-11）；Atomic Claim；15s Tombstone 区分 LATE_RESPONSE/UNMATCHED（IR-2）；重传仅更新 last_sent_at/attempt_count；容量 2048 + insert_failure 计数（IR-1）；binding_epoch 隔离（SR-4）；recordDeliveryLoss（修正 #2）；PARTIAL 响应按同 TxID 唯一候选匹配（SR-7/SR-8，多候选 → TRACKING_AMBIGUOUS）
- `dns_service_evaluator.hpp`：无状态纯函数；Missingness 三态（no_dns_observations / awaiting_inflight / insufficient_terminal_observations）；SR-9 突发单杀（同 epoch ≥3 超时 + 15s 窗口 + 无 known_success 穿插）；失败率/中位延迟阶梯；SR-8 分类覆盖率门禁

### 2. Policy 与兼容层
- `NetworkExperience` 增加 dns_service SLE + assessment_profile 字段
- `OverallPolicy`：保留 Phase 1 五参重载；六参版本实现 Reachability BAD > Reliability BAD > DNS BAD（仅 INTERNET_ACCESS 一票否决，Primary=DNS service resolution failure）> Responsiveness BAD > DEGRADED 阶梯（SR-5 依赖纯净性：DNS 不反向污染 IP Reachability）
- `LegacyAdapter`：HealthCheck JSON 升级 score_model=assurance_v2 + assessment_profile 字段；新增 toExperienceJsonV2（schema_version:2，network_health + service_health.dns）
- Metrics：MetricScope 增加 HOST/RESOLVER；MetricId 增加 DNS_QUERIES/DNS_FAILURE_RATIO/DNS_LATENCY_P50_MS/DNS_INFLIGHT；MetricsRegistry::publishScoped

### 3. eBPF capture（dns_monitor.bpf.c）
- raw_syscall tracepoint（sys_enter/sys_exit + arm64 syscall id 门控）：sendto/sendmsg/sendmmsg/recvfrom/recvmsg/recvmmsg
- dns_event ABI：direction/rcode/tc/malformed/fingerprint_quality/txid/qtype/qname_hash/timestamp
- perf_event_array dns_events；pending_recv LRU hash
- dns_capture_counters（PERCPU_ARRAY，13 项）：sendto/sendmsg/sendmmsg/recvfrom/recvmsg_enter/recvmsg_exit/recvmmsg_exit/msghdr_read_fail/iovec_read_fail/payload_read_fail/short_payload/emitted/emit_fail
- 保留 legacy 聚合 kprobe（dns_queries/dns_stats）作为兼容观测路径

### 4. 用户态 DnsMonitor
- perf_buffer 消费 + lost callback；drainEvents(tracker) 把事件归一化后交 Tracker
- getCaptureDiagnostics()：BPF capture counters + drain counters（poll/sample/lost/tracker_accepted）JSON 输出
- raw_syscall 程序显式 for_each attach（这是本次 NO_DNS_EVENTS 的根因修复）

### 5. 集成
- ServerContext 持有 dns_tracker + assessment_profile；DNS worker 周期 drain→sweep；每 30 tick 输出 dns-capture diag
- 网络质量线程与 D-Bus HealthCheck 均读取 Tracker → DnsServiceEvaluator → OverallPolicy
- 配置：dns.assessment_profile（NETWORK_ONLY|INTERNET_ACCESS，非法值告警回落 INTERNET_ACCESS）；weaknet-cli get/set 白名单同步；启动日志打印 profile
- SQLite：network_history 增量迁移 assessment_profile（DEFAULT 'UNSPECIFIED'）；写入 score_model=assurance_v2；queryHistory 输出 score_model/assessment_profile
- Web：DnsMonitor 下钻面板新增"DNS 服务保障"块（整体评级/DNS 主诊断/Profile，来自 /api/health 的 assurance_v2 字段）

### 6. 测试
- 新增 test_dns_transaction_tracker_gtest（8 用例）：Exactly-Once 与 RCODE 分类、TC/RCODE 互斥（修正 #4）、重传去重（first_sent_at 保持）、Timeout/Tombstone/LATE/UNMATCHED、epoch 隔离、Missingness 三态、SR-9 突发（成功穿插打断）、失败率+中位延迟
- x86 CTest 27/27 通过

## 二、真机验证记录（radxa@192.168.137.210，内核 5.15.147-99btf1-a733）

### 根因排查（NO_DNS_EVENTS → 已定位解决）
1. dns_events map 创建 EINVAL → 补 __type(key/value) 修复
2. recvmmsg 调 recvmsg 跨 section relocation → 独立函数体修复
3. **最终根因：raw_syscall 程序从未被 attach**（代码只显式 attach 两个 kprobe）→ for_each_program 显式 attach 后 `raw_syscall capture programs attached=8`
4. 修复后计数器链路（真机日志）：
   - capture: sendmsg=28303 recvmsg_enter=54495 recvmsg_exit=54495 emitted=53446
   - drain: sample_callbacks=53429 tracker_query_accepted=22352 tracker_response_accepted=31077
   - 板上 resolver 实际 syscall 路径为 sendmsg+recvmsg（sendto/sendmmsg 为 0）
   - msghdr/iovec/payload 读取失败均为 0 → syscall ABI 路线通过 capability gate，无需切换 TC/packet capture

### 事件丢失观测
- emit_fail≈2872 / lost≈2871（高频 syscall 时 perf buffer 容量不足）——已知边界，后续可调 buffer 页数
- short_payload=36871：非 DNS 的短 UDP/其他载荷被过滤（符合预期行为，计入观测）

### DNS 驱动整体评级（真机 HealthCheck）
```json
{"overall_quality":"POOR","overall_score":25.0,
 "issues":["DNS service resolution failure"],
 "assessment_profile":"INTERNET_ACCESS","score_model":"assurance_v2"}
```
- DNS SLE 独立进入 BAD，Overall 变 POOR，Primary Issue 正确
- SR-5 验证：IP Reachability 证据（rtt/tcp_loss）未受 DNS 故障污染

### 数据库
- 修复 WAL sidecar 权限（history.db-shm/-wal 属主混用导致 attempt to write a readonly database）后恢复写入（records=83480 持续增长）

### 故障注入
- iptables OUTPUT DROP udp/53 注入与清理均已执行（OUTPUT ACCEPT 复原）
- 注入期间 HealthCheck 保持 DNS failure 判定（当前 PARTIAL 事件噪声使 DNS 持续 BAD，见"已知边界"）

## 三、已知边界与后续工作

1. **PARTIAL 事件噪声**：capture 无法关联 fd→resolver endpoint（sys_enter_sendmsg 时 connect 细节不可见），query 与 response 均以 TxID-only PARTIAL 匹配。tracker_query_accepted 远大于真实 DNS query 数（recvmsg 方向的其他 UDP 流量也计入），failure_ratio 被稀释/抬高，DNS 判定偏保守（持续 BAD）。改进方向：connect/sendto fd→endpoint 关联表、按 payload_len/QR 位过滤、方向归一化强化
2. emit_fail/lost：perf buffer 8 页在高频下不足，建议调至 64+ 页并评估 wakeup
3. GetNetworkExperience 独立 D-Bus 方法（toExperienceJsonV2 已备好）尚未暴露
4. NetworkQualityChanged DNS 跃迁 exactly-once 未单独验证
5. DNS_BAD 稳定态下 SR-9 突发语义与 failure_ratio 路径的真机区分度需在 1 修复后复测
6. NXDOMAIN 语义（域名不存在 ≠ DNS 服务失败）已由 capture→Tracker 正确分类，SLE 语义待 1 修复后调阈值验证

## 四、部署速查

```bash
docker exec weaknet-arm64-dev bash -c 'cd /src && cmake --build build-arm64 --target weaknet-dbus-server history_query_tool ebpf -j1'
rsync -az -e ssh build-arm64/server/weaknet-dbus-server build-arm64/server/history_query_tool radxa@192.168.137.210:/home/radxa/weaknet/server/bin/
rsync -az -e ssh build-arm64/server/build/dns_monitor.bpf.o radxa@192.168.137.210:/home/radxa/weaknet/server/build/
ssh radxa@192.168.137.210 'sudo systemctl restart weaknet-server'
# 诊断
ssh radxa@192.168.137.210 'sudo journalctl -u weaknet-server | grep "dns-capture diag" | tail -1'
```
