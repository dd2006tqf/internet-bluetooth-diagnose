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
4. 远程运行 19 个 Google Test 单元测试套件
5. 远程运行功能测试（health/get/eBPF/指标）
6. 生成报告到 `ci-reports/`

### 跳过编译

```bash
./tools/ci.sh --skip-build
```

使用上次编译结果，只做部署+测试。适合只改了配置或没改代码时。

### 跳过部署

```bash
./tools/ci.sh --skip-deploy
```

只在本地容器内编译和跑单元测试，不部署到开发板。

### 只跑单元测试

```bash
./tools/ci.sh --unit-only
./tools/ci.sh --skip-build --unit-only   # 跳过编译
```

### 只跑功能测试

```bash
./tools/ci.sh --func-only
```

部署后只运行功能测试（health/get/eBPF/指标），不跑单元测试。

### 手动在开发板测试

```bash
ssh -t radxa@192.168.2.77 'sudo /home/radxa/weaknet/weaknet-test-full.sh'
```

## 环境变量

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `CONTAINER` | `weaknet-arm64-dev` | ARM64 构建容器名 |
| `BOARD` | `radxa@192.168.2.77` | 开发板 SSH 地址 |
| `JOBS` | `1` | 编译并行度（QEMU 下不要超过 1） |

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
- 19 个单元测试套件结果
- 功能测试指标（health JSON、eBPF 数量、RSSI、RTT、质量分数）
- 汇总统计

## 测试覆盖

### 单元测试（19 个套件，约 250 个用例）

| 套件 | 覆盖模块 |
|------|----------|
| test_net_info | NetInfo 数据序列化 |
| test_quality_assessor_gtest | 网络质量评估算法 |
| test_anomaly_detector_gtest | 流量异常检测 |
| test_audio_fusion_gtest | 蓝牙音频融合 |
| test_band_conflict_gtest | 2.4GHz 频段冲突 |
| test_serializer_gtest | 二进制序列化 |
| test_event_manager_gtest | 事件注册/分发 |
| test_bt_full_link_gtest | 蓝牙全链路集成 |
| test_iface_type_gtest | Wi-Fi 接口识别 |
| test_logger_gtest | 日志系统 |
| test_traffic_analyzer_gtest | 流量分析器 |
| test_bt_monitor | 蓝牙监控 |
| test_weak_netmgr_gtest | NetInfo + 质量评估 |
| test_net_iface_gtest | 网络接口检测 |
| test_rtt_monitor_gtest | RTT 质量评估 |
| test_bt_monitor_extra_gtest | 蓝牙数据结构 |
| test_jitter_monitor_gtest | 抖动监控 |
| test_dns_monitor_gtest | DNS 监控 |

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
A: 首次编译约 30 分钟（QEMU 模拟），之后 ccache 加速，增量编译只需几秒。用 `--skip-build` 可跳过编译。

### Q: 开发板连不上？
A: 检查 `ssh radxa@192.168.2.77 echo ok`。连不上会自动跳过远程测试。

### Q: 功能测试失败？
A: 确保服务端正常启动（`weaknet-test-full.sh` 会自动启动）。如果 `test-client get` 超时，是已知的 D-Bus 延迟问题。

### Q: 如何只测试某个模块？
A: 在开发板上直接运行单个测试：
```bash
ssh radxa@192.168.2.77 '
export LD_LIBRARY_PATH=/home/radxa/weaknet/lib:/home/radxa/weaknet/client/lib
cd /home/radxa/weaknet/server
./test/bin/test_quality_assessor_gtest
'
```
