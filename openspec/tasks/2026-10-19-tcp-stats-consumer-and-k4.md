# O1-A + O2-A + K4：TCP 管线真实消费者 + CLI stats + 板端补采

> 日期：2026-10-19
> 分类：工作流外实施（full-code-review 修复批次 1）
> 结果：已完成

## 构思

全仓评审发现三块缺陷（均经用户确认按"接真实消费者、不删除"路线处置）：

- **O1（选 A）**：`tcp_retrans` 与 `tcp_conn` 两条 eBPF 管线零真实消费者——
  不进 D-Bus、不进 CLI、不进 DB 历史、不上行。此前的评审结论是"编译即完成"，
  但按铁律"无真实消费者的生产接口是孤儿"，必须接线或移除。选 A（接线）：
  两条管线的采样能力已板端验证，删掉浪费。
- **O2（选 A）**：一批只读 D-Bus 方法（GetTcpRetransStats/GetTcpConnStats 及
  既有 domain 读方法）零 CLI 调用者。选 A：新增 `weaknet-cli stats <domain>`
  薄透传子命令，把全部只读 domain 读方法一次接通。
- **K4**：telemetry 快照缺 `hardware_arch`/`os_kernel`，信封缺
  `location`/`rtt_interval_seconds`/`rtt_probe_targets`——云端契约有列但板端
  不填，属"契约有列无采集"的半截实现。

不走 OpenSpec change：均为评审修复批次，用户明确要求工作流外实施并逐批提交。

## 实现方式

**O1-A 接线（7 层）**：
- `common.hpp` + `dbus_service.{hpp,cpp}`：新增 `GetTcpRetransStats`
  （shared_lock 下 computeLossRate + getTopRetransConnections(10)）与
  `GetTcpConnStats`（getStats → TcpConnAggStats JSON），监控缺席时诚实返回
  error JSON。
- `client/{weaknet_client.h,client.cpp}`：C API
  `weaknet_get_tcp_retrans_stats` / `weaknet_get_tcp_conn_stats`（4 参模式）。
- `metrics/{metric_types.hpp,metrics_registry.hpp}`：3 个新 MetricId
  （TCP_RETRANS_LOSS_PCT / TCP_CONN_ACTIVE / TCP_CONN_ACCEPT_RATE）+
  `MetricsRegistry::global()` 单例（monitor 线程发布、DB 持久化线程读取）。
- `server.cpp`：tcp_retrans/tcp_conn 采样线程 publishGlobal；tcp_conn 线程
  补 emitEbpfStats；uplink 第 4 采样器 lambda（shared_lock 下读两 monitor）。
- `edge_wireless_uplink_exporter.{hpp,cpp}`：TcpLossSample 结构 +
  env_window 新 kind `tcp_loss_agg`（云存储 snapshots_json 不限型，免契约变更）。
- `database_manager.cpp`：3 新列
  （tcp_retrans_loss_pct REAL / tcp_conn_active / tcp_conn_accepts），从全局
  registry 读、freshness 门控、未观测写 JSON NULL（≠ 0，诚实空值纪律）；
  queryHistory SELECT 与 JSON 同步扩展。
- `tcp_conn_monitor.{hpp,cpp}`：补 keyStatsSnapshot() 驱逐可见性覆盖。

**O2-A**：`weaknet_cli.cpp` 新增 `stats` 子命令（prog 参数 21→22），
`callStats(domain)` 薄透传 10 个 domain：dns/wifi-loss/http/process/
skb-drop/tcp-rtt/tcp-retrans/tcp-conn/ebpf-health/history。

**K4**：
- `edge_telemetry_serializer.hpp`：快照内补 `hardware_arch`/`os_kernel`
  （uname()，失败空串诚实兜底）。
- `edge_telemetry_exporter.cpp`：信封补 `location`（site_id 兜底 dev_id）、
  `rtt_interval_seconds`（interval_ms/1000，clamp ≥0.001 保契约 gt=0）、
  `rtt_probe_targets`（数组单元素）。

**验证器修正**：`verify_telemetry_contract.py` 的 `_extract_top_level_keys`
正则原要求引号紧邻字段名，`json << ",\"field\":"` 逗号先行写法整段漏抓，
K4 字段从报告消失。改为引号前允许反斜杠、逗号先行可匹配。

**测试**：4 个新 gtest——PersistsTcpLossAggregateColumns（NULL 语义 +
落库）、EmitsTcpLossAggWhenPresent / OmitsTcpLossAggWhenAbsent（上行契约）、
EmitsHardwareArchAndOsKernel（K4 快照）。

## 结果

- x86 全量 ctest **52/52 通过**（含 4 新用例）。
- 契约校验 `verify_telemetry_contract.py --strict` → `OK edge-telemetry-contract`；
  `--selftest` → drift 检测正常。
- 修复器正则修正后报告正确列出 hardware_arch/os_kernel。
- 遗留（后续批次）：O3-B 信号停发需先解耦测试 mock 计数；O11-O16 云侧
  真实消费者；板端部署验证待全部批次完成后统一 `BOARD=board ./tools/ci.sh`。
