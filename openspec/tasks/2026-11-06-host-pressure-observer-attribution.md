# 采集端 CPU 压力接入观测者降级归因（HOST 域首个真实消费者）

> 日期：2026-11-06
> 分类：工作流外实施
> 结果：已完成（x86 编译 + 56 套件全绿 + 边云契约校验通过；未上真机）

## 构思

### 要解决什么

`kmod/weaknet_cpu_stat` 是 B1 实验的产物（见 `2026-11-05-kmod-b1-experiment.md`），
已在板端跑通，但**在生产链路里没有任何消费者**。同时诊断侧存在一个真实的
证据盲区：

`DnsServiceEvaluator` 的 observer 质量门禁只看四个 eBPF 传输层失败率
（`capture_emit_failure_ratio` / `perf_delivery_loss_ratio` / `ambiguity_ratio` /
`insert_failure_ratio`）。四项任一超阈值就判 `observer_unreliable_*`，SLE 转
`UNKNOWN`。但「观测不可靠」有两个成因：

- (a) 链路侧真的出了问题（内核拥塞、perf buffer 来不及消费、跟踪表满）；
- (b) 采集端自己 CPU 被打满，采集线程抢不到 CPU，本该产出的事件没产出。

旧证据对 (a)(b) 完全同形。后果双向：把 (b) 报成网络故障，现场去查网络查不出问题；
把 (b) 报成「观测不可靠」，真实原因（本机过载）永远不被暴露。

### 为什么不做进 OpenSpec change

本次是在**已存在的实验模块之上补消费者**，代码规模小、表面清晰。按 CLAUDE.md
「规划与实现分离」，先写代码再开 change 追认会因 footprint 为 0 /
`planning changed during command` 而报废。因此走工作流外任务记录。

### 备选方案与取舍

| 方案 | 做法 | 取舍 |
| --- | --- | --- |
| A（选定）新增第 2 层归因模块 | 保留 `DnsServiceEvaluator` 纯函数契约，在 `decide()` 之后追加独立归因 | 观测者状态不污染被观测对象判定；SR 同源纪律与可判定性保住 |
| B 把 CPU 值传进 `DnsServiceEvaluator` | 在评估器内部判 CPU | 同一 DNS 窗口在板子忙/闲时结论不同，评估器不再是纯函数，单元测试失效 |
| C 归因直接改 OverallPolicy | 让 CPU 过载参与评分/veto | 越界：过载是本机运行状态，不是被评估链路的健康度 |

## 实现方式

### 新增文件

| 文件 | 作用 |
| --- | --- |
| `server/include/assurance/observer_attribution.hpp` | 四值归因枚举 + 判定函数声明 + 语义边界（文件头长注释） |
| `server/src/assurance/observer_attribution.cpp` | 判定实现：NONE / UNATTRIBUTED / SELF_INFLICTED / NOT_HOST_CPU_BOUND，阈值 0.85 |
| `server/include/host_pressure_monitor.hpp` | CPU 压力采样器与线程声明 |
| `server/src/host_pressure_monitor.cpp` | 打开 `/dev/weaknet_cpu_stat` → seqlock 重试读 → 区间差值 → 加权比率 → 双发指标 |
| `server/test/unit/test_observer_attribution_gtest.cpp` | 归因真值表 + 采样器降级语义 |

### 改动文件

| 文件 | 关键改动 |
| --- | --- |
| `server/include/metrics/metric_types.hpp` | 新增 `MetricId::CPU_BUSY_RATIO`（`MetricScope::HOST` 首个实际使用指标，GAUGE，freshness 15s/30s） |
| `server/include/assurance/network_experience.hpp` | `NetworkExperience` 增加 `observer_attribution` / `observer_attribution_detail`，默认 NONE |
| `server/src/server.cpp` | 在 `decide()` 之后读 `host:` 域采样并归因；**证据挂回 `exp.dns_service.evidence`（append）** |
| `server/include/assurance/legacy_adapter.hpp` | `toExperienceJsonV2` 序列化两个新字段（否则 `GetNetworkExperience` 读不到） |
| `server/include/assurance/edge_telemetry_serializer.hpp` | 边云遥测序列化同样字段（走 `escapeJsonString`） |
| `Large-Model-Application/.../contracts.py` | `NetworkExperienceSnapshot` 声明两个字段（`extra="forbid"` 时必须两侧同步，否则整批遥测被拒收） |
| `server/include/weaknet_config.hpp` / `server/src/weaknet_config.cpp` | 新增 `host_pressure` 配置块；补齐 `applyMonitorField`（yaml）、`applyMonitorParam`（CLI set）、`get/setMonitorEnabled`、`serializeMonitorJson`、有效名集合 |
| `server/src/monitor_plugins.cpp` | 新增 `HostPressurePlugin`（order 10），注册 `host_pressure` |
| `server/include/server.hpp` / `server/include/logger.hpp` | `host_pressure_stop` 停止标志；`LogModule::HOST_PRESSURE` |
| `client/weaknet_cli.cpp` | `list` 与 `--help` 补 `host_pressure` |
| `config.yaml` | 新增 `monitors.host_pressure`（enabled: true, interval_ms: 15000） |
| `server/test/CMakeLists.txt` | 注册 `test_observer_attribution_gtest` |
| `server/test/unit/test_weaknet_config_gtest.cpp` | 4 个新测试：yaml 解析、区间拒绝、未知字段拒绝、序列化 |
| `server/test/unit/test_w4_snapshot_lifecycle_gtest.cpp` | 1 个新测试：归因字段必须真正走出进程（v2 JSON） |

### 关键决策

**1. 独立插件而非塞进 quality 线程。** quality 是唯一 evaluator 执行点，它的执行
时长本身就是被观测对象的一部分。把「测量采集端压力」放进 quality 线程，quality 忙到
跑不动时也无法采样——观测者不能被自己的观测对象挡住。

**2. 归因分两层，绝不改第一层。** `DnsServiceEvaluator` 保持「只吃 DNS 事实的无状态
纯函数」。第 2 层只回答「那份观测为什么不可靠」，不修改任何 state/coverage/reason。

**3. 双发布指标。** `ctx->metrics_registry`（评估侧读取）与 `MetricsRegistry::global()`
（SQLite 持久化侧读取，`database_manager.cpp` 的 `insertSnapshot` 只查 global）都要发。
只发前者会让 CPU 压力永远进不了 SQLite——「看起来接好了、实际永远 NULL」的典型陷阱。
这与 `server.cpp` 里 TCP 重传/连接指标的处理是同一纪律。

**4. `attr.evidence` 必须挂回 SLE。** `ObserverAttributionResult::evidence` 的注释
写的是「直接挂到 SleResult.evidence」，但此前只生成不使用，Build 出来的 evidence
被整体丢弃，于是 `host_cpu_busy_ratio` 永远进不了任何序列化出口，`buildResolver`
（只认 SLE evidence）也无法用它做前置条件。挂到 `exp.dns_service` 而不是局部
`dns_sle`：`decide()` 已按值拷贝过 SLE，改局部对已发布的快照无效。用 append 而非
覆盖：observer 原有的四个比率证据必须保留。

**5. 不改跨端指纹。** 曾尝试把 `host_pressure.interval_ms` 加进
`trialableKeyNames()`，导致 `test_action_catalog_version_gtest` 从
`703e0ceb4b62` 漂到 `62cea8d435b4`、key 数 42 → 44。该白名单与云端
`edge_action_catalog.CONFIG_KEYS` 跨端绑定，不匹配会让网关 fail-closed、整片现场
无法下发任何调参动作。因此**回退**：`config.yaml` 与 D-Bus `weaknet-cli set/get`
两条路径保持可用，但不进云端可下发目录。若要开放云端下发，需要与云端契约同步
变更并重新冻结指纹。

**6. 缺数据绝不伪造 0。** 设备节点不存在时发 `MetricSample::unavailable()`，
归因落在 `UNATTRIBUTED`。0 会被下游解读成「CPU 很闲」，从而掩盖真实故障。

### 验证手段

```bash
cmake --build build-x86 -j$(nproc)
ctest --test-dir build-x86/server --output-on-failure        # 56/56 通过
Large-Model-Application/.venv/bin/python tools/verify_telemetry_contract.py
# → OK edge-telemetry-contract
Large-Model-Application/.venv/bin/python -m pytest -k 'network or telemetry or contract'
# → 106 passed, 1 skipped
Large-Model-Application/.venv/bin/python -m pytest .../test_edge_action_catalog.py
# → 9 passed（确认指纹未被改动）
```

## 结果

- 四个孤儿/断开的表面被接通：配置入口（yaml 之前会整份解析失败）、归因字段
  （写的没人序列化）、归因证据（生成后被丢弃）、指标持久化（只进评估侧注册表）。
- 缺 `/dev/weaknet_cpu_stat` 时行为与引入前完全一致，现场零风险。
- 58 个测试目标全绿（56 套件），边云契约双向校验通过，跨端指纹 `703e0ceb4b62`
  未变。

**遗留 / 后续值得升格为 OpenSpec change 的部分**：

1. 云端可试行目录（`trialableKeyNames` / `CONFIG_KEYS`）尚未纳入 `host_pressure.*`，
   云端目前无法下发这两个键的调参动作。需要两侧同步变更 + 重新冻结指纹。
2. 归因只覆盖 DNS 一个 SLE。其它 SLE 若引入 observer 门禁，需要在 `server.cpp`
   的归因块里并列追加。
3. 未上真机验证。板端 `/dev/weaknet_cpu_stat` 存在时的实测归因结果、以及
   `weaknet-cli get host_pressure` 的真实往返，都需要上板确认。
4. `kmod/weaknet_cpu_stat.ko` 仍不在 OTA manifest 里，属于设计边界；本次只消费
   共享 ABI 头，不打包 .ko。
