# WeakNet W3 参数分类与校准规范（Calibration & Holdout）

本文档按照 Assurance v1 架构规范，将全系统参数划分为三类，并规划 Active 与 Passive 分离的 **Calibration Set（校准集）** 与 **Holdout Set（验证集）**。

---

## 一、三类参数分类冻结

### A. 评价模型阈值（通过实测数据校准，反映用户真实体验分界）

| 模块 / 维度 | 参数项 | 现有工程初值 | 校准依据与目标 | 备注 |
|---|---|---|---|---|
| **DNS Service** | `error_ratio_degraded` | 5% (0.05) | 事务失败率达到轻微可感知影响阈值 | 递归/权威偶尔偶发波动 |
| | `error_ratio_bad` | 20% (0.20) | 事务失败率导致严重无法解析网络 | 主机解析能力大面积受损 |
| | `median_latency_degraded_ms` | 150.0 ms | 实测跨网递归常在 80~180ms，初值偏紧 | 拟调整至 200.0 ms |
| | `median_latency_bad_ms` | 500.0 ms | 典型业务严重等待卡顿分界 | 维持 500.0 ms |
| | `min_terminals_for_evaluation` | 5 事务 | 排除单笔偶发噪点的最小样本数 | 维持 5 |
| | `critical_timeout_count` | 3 次 | SR-9 突发连续超时单杀门槛 | 维持 3 |
| | `critical_window` | 15000 ms (15s) | SR-9 突发超时的连续判定时间窗口 | 维持 15s |
| **IP Reachability** | `consecutive_fails_bad` | 3 次 | 尾部连续 ping 探测失败判定 BAD | 维持 3 |
| | `consecutive_fails_degraded` | 1 次 | 偶发单次丢包判定 DEGRADED | 维持 1 |
| | `success_rate_bad` | < 50% | 整体连通成功率底线 | 维持 50% |
| | `success_rate_degraded` | < 90% | 整体连通成功率警戒线 | 维持 90% |
| **Responsiveness** | `rtt_median_good` | <= 60.0 ms | 良好交互体验时延上限 | 维持 60ms |
| | `rtt_median_bad` | > 150.0 ms | 明显卡顿交互时延下限 | 维持 150ms |
| | `jitter_median_good` | <= 15.0 ms | 抖动良好上限 | 维持 15ms |
| | `jitter_median_bad` | > 40.0 ms | 严重抖动下限 | 维持 40ms |
| **Reliability** | `wifi_loss_bad` | >= 3.0% | 空口损伤严重 | 维持 3% |
| | `tcp_loss_bad` | >= 4.0% | 传输层重传损伤严重 | 维持 4% |
| **TCP Connect** | `fail_ratio_bad` | >= 20.0% | 建连大面积失败 | 维持 20% |
| | `median_latency_bad_ms` | >= 1000.0 ms | 握手极其迟缓 | 维持 1000ms |
| **Cleartext HTTP** | `5xx_ratio_bad` | >= 20.0% | 服务端故障为主 | 维持 20% |
| | `no_response_ratio_bad` | >= 30.0% | 请求无响应大面积发生 | 维持 30% |
| **Captive Portal** | `portal_min_domains` | 2 独立故障域 | 达成 Quorum 确诊的最少故障域数 | 维持 2 |
| **Active Capability**| `min_eligible_targets`| 2 独立目标 | 授予 Capability 判定所需最少目标数 | 维持 2 |

---

### B. 安全不变量（核心架构契约，严禁用数据拟合修改）

1. **Single-Source Capture 原则**：Query 与 Response 各自由单一挂点、单一已定型数据包提取，严禁跨探针时序 Join。
2. **No Evidence No Progression (HR-6)**：无新 Evidence 到达时，状态机与滞后计数器绝不向前推进。
3. **依赖截断与根因纯净性**：
   - 上游 DNS 失败 → 下游 TCP/TLS/HTTPS 标记为 `blocked_by_dns`，绝不重复记假失败。
   - 上游 TCP 失败 → 下游 TLS/HTTPS 标记为 `blocked_by_tcp`。
   - DNS 故障绝不反向污染 IP Reachability。
4. **Quorum 与故障域机制**：
   - Captive Portal 必须在 >=2 个**独立故障域**给出一致信号时才确诊。
   - 单端点异常仅置 `portal_suspected_single_endpoint`，不判 BAD。
5. **Transport 可用性与状态码解耦**：收到任意合法 HTTP 状态码（含 404/500）均证明 HTTPS Transport 存在，不得因 4xx/5xx 把 Internet 判为 BAD。
6. **Direct 证据优先与 Absence-derived 降级**：
   - 明确错误证据（SERVFAIL/REFUSED 等）在 PARTIAL 观测质量下依然有效。
   - 缺席推导证据（TIMEOUT）在 Observer 损坏时降级为 UNKNOWN，绝不嫁祸业务。
7. **底层不健康抑制门户判决**：底层链路不通时，禁止宣称门户，将解释权归还底层 SLE。
8. **状态决定分数区间**：display_score 仅作单向兼容投影，绝不反向决定状态。
9. **Active Probe 不污染 Passive 证据**：仅运行探测时，Passive DNS/TCP/HTTP 计数增量恒为 0。

---

### C. 实现与时间行为参数（系统吞吐、内存与时间动力学控制）

| 参数名 | 取值 | 作用与物理约束 |
|---|---|---|
| `active_probe.interval_ms` | 30000 ms (30s) | 探测周期，保证背景探针不成为网络负担 |
| `active_probe.timeout_ms` | 3000 ms (3s) | 单次阶段超时（DNS/TCP/TLS 分别受此限制） |
| `state_stabilizer.degradation_hold` | 10000 ms (10s) | 劣化确认窗口，过滤短暂偶发毛刺 |
| `state_stabilizer.recovery_hold` | 20000 ms (20s) | 恢复稳态窗口，防止网络震荡时 Flapping |
| `dns_self_endpoints.capacity` | 256 条 | LRU_HASH 容量，足以覆盖探测并发 |
| `dns_self_endpoints.ttl` | 30000000000 ns (30s) | 覆盖 probe 超时 (3s) + 迟到墓碑宽限 (15s)，超时自动释放端口豁免 |
| `dns_capture_pages` | 64 页 (256 KB) | 高频事件 perf buffer 容量 |
| `dns_tracker.capacity` | 2048 事务 | 内存追踪容量上限 |
| `dns_tracker.tombstone_ttl` | 15000000000 ns (15s) | 墓碑保留时长，区分 LATE_RESPONSE 与 UNMATCHED |

---

## 二、测试数据集规划（Calibration vs Holdout）

针对 12 种标准网络场景，Active 与 Passive 完全隔离，切分为**校准集**（用于发现分界点并确定阈值）与**验证集**（严格盲测，禁止反馈调参）：

| 场景序号 | 场景描述 | Calibration Set 驱动条件 | Holdout Set 验证判据 | 考察指标 |
|---|---|---|---|---|
| **S1** | **基线健康** | 真实优质网络，RTT<30ms, 无丢包, DNS<50ms | Overall=GOOD, Coverage=FULL, Issues=[] | FP=0 |
| **S2** | **轻度延迟** | 注入 80~120ms 时延，无丢包 | SLE 保持 GOOD，允许 warning，不降 BAD | 容忍轻度抖动 |
| **S3** | **持续中延迟** | 注入 160~240ms 时延，持续 30s | DNS/Resp 进入 DEGRADED，Overall=DEGRADED | 检测时间 <=15s |
| **S4** | **严重延迟卡顿** | 注入 600~900ms 延迟，持续 30s | DNS/Resp 判定 BAD，Overall=DEGRADED 或 BAD | 准确分界 |
| **S5** | **轻度偶发丢包** | 注入 1%~2% 丢包（单发偶发） | Reliability 维持 DEGRADED，Overall 不直接 BAD | 避免过度敏感 |
| **S6** | **高比例严重丢包**| 注入 15%~30% 丢包，持续 20s | Reliability=BAD, Overall=POOR/BAD | FN=0 |
| **S7** | **突发单杀 (SR-9)**| 连续 3 笔 DNS 超时（15s 内且无已知成功穿插）| DNS 立即触发 Critical Bypass 跃迁 BAD | 发现时延 <=3s |
| **S8** | **单目标失效** | Active 探测中仅 1 个目标超时/挂掉，其余 2 个正常 | Overall 绝不判 BAD，记录 Warning | Quorum 正确 |
| **S9** | **全目标失效** | Active 探测中所有目标同时连接失败 | Active Capability BAD，Overall 一票否决 | 强否决权确立 |
| **S10** | **短暂瞬时毛刺** | 注入 1 次 800ms 或单笔丢包（持续 2 秒恢复） | 被 StateStabilizer 吸收，输出状态不产生 Flapping | 抑制抖动 |
| **S11** | **持续硬性故障** | 拔网线或 DROP 全部数据流持续 60s | 10s 恶化保持期满后稳固进入 BAD，无回退 | 推进时间符合预期 |
| **S12** | **故障后恢复** | 恢复网络，连续注入正常样本持续 40s | 20s 恢复保持期满后平稳回到 GOOD | 恢复时间 <=25s |

---

## 三、退出条件与量化准则

1. **False Positive (误报率)**: S1、S2、S8 场景下，Internet BAD 误报率为 **0%**。
2. **False Negative (漏报率)**: S6、S7、S9、S11 场景下，故障漏报率为 **0%**。
3. **Flapping Rate (震荡率)**: S10 短暂毛刺下，状态跃迁次数为 **0 次**。
4. **Detection Time**: S7 突发单杀 <= 3s，普通严重故障稳定器推进时间在 10s ± 2s。
5. **Recovery Time**: 故障消除后恢复 GOOD 稳定时间在 20s ± 3s。
6. **No Evidence No Progression**: 无新数据时状态与计数绝对静止。
