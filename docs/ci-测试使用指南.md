# CI 与测试使用指南

## 快速开始

```bash
./tools/ci.sh              # 一键编译→部署→测试→生成报告
```

## 用法

### 完整流程（日常使用）

```bash
./tools/ci.sh
```

自动完成 5 步：
1. 在 ARM64 容器内编译（ccache 加速）
2. 打包部署目录（dist-arm64/）
3. rsync 部署到开发板
4. 远程运行功能测试（health/get/eBPF/指标/历史）
5. 生成报告到 `ci-reports/`

> **单元测试不在开发板上跑**。39 个 gtest 套件的归属是 x86：
> `ctest --test-dir build-x86/server`（本地，也是 `.ai-harness/project-profile.json`
> 的 `test-all` 命令）与 GitHub Actions。板端只部署 `test_ebpf`
> （需要 root + 内核 eBPF，只能真机跑），由 `weaknet-test-full.sh` 的 Phase 11 执行。

### 使用已有 ARM64 构建结果

当前 `ci.sh` 每次执行都会在 `build-arm64/` 中进行 CMake 增量配置/构建；未修改的目标会由 CMake/ccache 复用。部署包始终从该目录重新整理到 `dist-arm64/`。

### 跳过部署

```bash
./tools/ci.sh --skip-deploy
```

只在本地容器内编译并打包 `dist-arm64/`，不部署到开发板。

> 注意：`ci.sh` **不在本地跑单元测试**。本地跑单测请直接用
> `ctest --test-dir build-x86/server`（x86，39 个套件）或容器内
> `ctest --test-dir build-arm64/server`。

### 完整参数列表

`ci.sh` 只接受以下参数（其它参数会以"未知参数"退出）：

| 参数 | 作用 |
|------|------|
| `--commit` | 归档后使用：先 git commit + push，再编译部署测试 |
| `--local-only` | 只编译打包，不部署、不推送、不测试 |
| `--skip-push` | 配合 `--commit`：只提交不推送 |
| `--skip-deploy` | 跳过部署步骤 |
| `--skip-test` | 跳过开发板测试步骤 |

### 手动在开发板测试

```bash
ssh -t radxa@radxa-cubie-a7a.local 'sudo /home/radxa/weaknet/weaknet-test-full.sh'
```

## 环境变量

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `CONTAINER` | `weaknet-arm64-dev` | ARM64 构建容器名 |
| `BOARD` | `radxa@radxa-cubie-a7a.local` | 开发板 SSH 地址 |

> 编译并行度在 `ci.sh` 内**硬编码为 `-j1`**（QEMU 模拟下高并行度会导致编译器
> segfault），不提供 `JOBS` 环境变量。

示例：
```bash
BOARD=radxa@192.168.1.100 ./tools/ci.sh
```

## 测试报告

每次运行会在 `ci-reports/` 下生成带时间戳的报告：

```
ci-reports/
└── ci_20260819_155927.txt
```

报告包含：
- 编译状态
- 功能测试指标（health JSON、eBPF 数量、RSSI、RTT、质量分数）
- 汇总统计

## 测试覆盖

### 单元测试（39 个套件，约 386 个用例）

跑法：`ctest --test-dir build-x86/server --output-on-failure`

| 套件 | 覆盖模块 |
|------|----------|
| test_net_info | NetInfo 数据模型与 JSON/二进制序列化 |
| test_metrics_registry_gtest | MetricsRegistry 窗口发布 + CounterNormalizer |
| test_sle_evaluator_gtest | 核心 SLE 阈值（Reach/Resp/Rel/RF）+ coverage gate + HR-6 |
| test_dns_transaction_tracker_gtest | DNS 事务配对、epoch 隔离、歧义与容量 |
| test_dns_service_evaluator_gtest | DNS SLE 真值表（Missingness/互斥/SR-9/Observer/NXDOMAIN） |
| test_tcp_connect_evaluator_gtest | TCP Connect SLE 阈值与观测门禁 |
| test_http_access_evaluator_gtest | HTTP Access SLE（4xx 非坏、5xx/无响应） |
| test_captive_portal_evaluator_gtest | Captive Portal 门禁与底层健康约束 |
| test_assurance_policy_source_gtest | OverallPolicy 否决权与 Active/Passive 边界 |
| test_active_connectivity_gtest | 受控主动探测 quorum / 依赖截断 / 能力缺失 |
| test_tls_probe_client_gtest | TLS 探针（状态码解耦、畸形响应、超时） |
| test_w3_time_dynamics_gtest | 时间动力学（10s 恶化 / 20s 恢复 / critical bypass） |
| test_w4_fault_matrix_gtest | 全故障矩阵锁定 |
| test_w4_snapshot_lifecycle_gtest | 快照生命周期（no_assessment_yet/stale） |
| test_edge_telemetry_exporter_gtest | 上报契约、签名字节、pending_actions、看门狗回滚 |
| test_diagnosis_engine_gtest | 诊断引擎 golden cases + 谓词三态 + 抑制环 |
| test_quality_assessor_gtest | LegacyAdapter 状态→分数投影 |
| test_audio_fusion_gtest | 蓝牙音频融合评分 |
| test_band_conflict_gtest | 2.4GHz 频段冲突检测 |
| test_bt_monitor_extra_gtest | 蓝牙数据结构 |
| test_serializer_gtest | 二进制序列化与安全文件路径 |
| test_event_manager_gtest | 事件注册/分发 |
| test_bt_full_link_gtest | 蓝牙全链路集成 |
| test_bt_monitor | 蓝牙纯逻辑（RSSI 分档/距离估算/评分边界） |
| test_iface_type_gtest | Wi-Fi 接口识别（sysfs 模拟） |
| test_logger_gtest | 日志系统与旧日志清理 |
| test_traffic_analyzer_gtest | 流量分析器降级模式 |
| test_weak_netmgr_gtest | NetInfo + 质量评估 |
| test_net_iface_gtest | 网络接口检测 |
| test_rtt_monitor_gtest | RTT 质量评估 |
| test_jitter_monitor_gtest | 抖动指标（数据模型 + evaluator；监控器已合并进 rtt） |
| test_dns_monitor_gtest | DNS 监控 |
| test_database_manager_gtest | SQLite 持久化与 schema |
| test_network_epoch_store_gtest | network_epoch 跨重启递增 |
| test_net_wifiriss_gtest | wpa_cli / /proc 回退解析 |
| test_ebpf_monitor_observability_gtest | eBPF 监控器公共契约与读计数 |
| test_weaknet_config_gtest | YAML 解析、白名单、ConfigTransaction |
| test_monitor_registry_gtest | 插件注册表 |
| test_monitor_manager_gtest | 插件生命周期与依赖拓扑 |

### 功能测试

| Phase | 测试内容 |
|-------|----------|
| 1 | 启动服务端 |
| 2 | health / get |
| 3 | 基础功能 |
| 4 | Ping 测试 |
| 5 | 事件系统 |
| 6 | 网络质量 |
| 7 | 蓝牙 |
| 8 | 错误处理 |
| 9 | 性能 |
| 10 | 单项命令验证 |
| 11 | 单元测试（GTest） |
| 12 | eBPF 测试 |
| 13 | eBPF 挂载检查 |
| 14 | 服务端日志 |

## 文件结构

```
tools/
├── ci.sh                    # 唯一入口
└── weaknet-test-full.sh     # 板端功能测试脚本

ci-reports/
└── ci_<时间戳>.txt          # 测试报告（不入 git）
```

## 常见问题

### Q: 编译超时怎么办？
A: 首次编译约 30 分钟（QEMU 模拟），之后 ccache 加速，增量编译只需几秒。`ci.sh` 每次都会执行增量 CMake 构建，未修改的目标由 CMake/ccache 复用；只想打包不部署可用 `--local-only`。

### Q: 开发板连不上？
A: 检查 `ssh radxa@radxa-cubie-a7a.local echo ok`。连不上会自动跳过远程测试。

### Q: 功能测试失败？
A: 确保服务端正常启动（`weaknet-test-full.sh` 会自动启动）。如果 `test-client get` 超时，是已知的 D-Bus 延迟问题。

### Q: 如何只测试某个模块？
A: 分两种情况。

**设计为可单测的模块（绝大多数）在 x86 本地跑**，用 ctest 的正则过滤：

```bash
ctest --test-dir build-x86/server -R test_dns_service_evaluator --output-on-failure
```

**只有需要真机内核的 test_ebpf 在开发板跑**（板端 `server/test/` 下也只部署了它）：

```bash
ssh radxa@radxa-cubie-a7a.local '
export LD_LIBRARY_PATH=/home/radxa/weaknet/lib:/home/radxa/weaknet/client/lib:/usr/local/lib
cd /home/radxa/weaknet/server
sudo ./test/test_ebpf
'
```
