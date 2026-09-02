# WeakNet CLI (weaknet-cli) 使用手册

> **版本**：v1.0  
> **适用平台**：Radxa Cubie A7A (ARM64) / x86_64 开发机  
> **依赖**：systemd、D-Bus 系统总线、libweaknet.so

---

## 1. 概述

`weaknet-cli` 是 WeakNet 网络诊断平台的**命令行配置管理工具**，用于在开发板上**实时查询、修改监控器参数**，无需重启服务。它是 `libweaknet.so` C API 的薄封装，通过 D-Bus 系统总线与 `weaknet-dbus-server` 通信。

**核心能力**：
- 🔍 **查询**：实时获取任意监控器的完整参数（JSON 格式）
- ⚙️ **设置**：实时修改监控器参数（白名单校验、区间校验、原子提交）
- 📋 **列举**：列出所有可配置的监控器名称

> **关键特性**：运行时修改**仅影响内存态**，重启服务后自动回落到 `/etc/weaknet/config.yaml` 的启动配置。这是设计使然——配置文件是“启动快照”，CLI 是“运行时覆盖”。

---

## 2. 安装与前置条件

### 2.1 开发板部署（ARM64）

`weaknet-cli` 随 `tools/ci.sh` 自动打包进 `dist-arm64/client/bin/weaknet-cli`，并由 `tools/ci.sh` 部署到开发板：

```bash
# 开发板上的路径
/home/radxa/weaknet/client/bin/weaknet-cli
```

**依赖**：
- `libweaknet.so` 必须在系统库路径（`/usr/local/lib/`），由 `tools/ci.sh` 自动部署并 `ldconfig`
- D-Bus 系统总线（服务端运行在系统总线 `com.example.WeakNet`）

### 2.2 开发机验证（x86_64）

```bash
# 本地编译（不含 eBPF）
cmake -B build-x86 -DCMAKE_BUILD_TYPE=Debug -DBUILD_EBPF=OFF
cmake --build build-x86 -j$(nproc)

# 二进制位置
./build-x86/client/bin/weaknet_cli
```

> 注意：x86 版本需连接开发板的 D-Bus（或本地运行服务端）才能工作。

---

## 3. 命令参考

### 3.1 语法概览

```bash
weaknet-cli <command> [arguments...]

命令：
  get <monitor>          查询监控器当前参数（JSON）
  set <key> <value>      设置参数（白名单校验）
  list                   列出所有可用监控器名
```

### 3.2 `weaknet-cli list`

列出所有可配置的监控器名称。

```bash
$ weaknet-cli list
rtt
jitter
rssi
tcp_loss
traffic
quality
bluetooth
dns
wifi_loss
http_latency
process_profiler
tcp_retrans
tcp_conn
server
all
```

**输出**：每行一个监控器名，最后一行 `all` 表示查询全部。

---

### 3.3 `weaknet-cli get <monitor>`

查询指定监控器的完整当前参数（JSON 格式）。

```bash
# 查询单个监控器
$ weaknet-cli get rtt
{"rtt":{"enabled":true,"target":"223.5.5.5","interval_ms":10000,"timeout_ms":800}}

# 查询全部监控器
$ weaknet-cli get all
{
  "server":{"data_dir":"/home/radxa/weaknet/data","log_level":"info"},
  "rtt":{"enabled":true,"target":"223.5.5.5","interval_ms":10000,"timeout_ms":800},
  "jitter":{"enabled":true,"target":"223.5.5.5","interval_ms":2000,"timeout_ms":800,"window_size":30},
  ...
}
```

**支持的监控器名**：`rtt` `jitter` `rssi` `tcp_loss` `traffic` `quality` `bluetooth` `dns` `wifi_loss` `http_latency` `process_profiler` `tcp_retrans` `tcp_conn` `server` `all`

**输出字段说明**（以 `rtt` 为例）：

| 字段 | 类型 | 说明 |
|---|---|---|
| `enabled` | bool | 是否启用该监控器 |
| `target` | string | 探测目标 IP/域名 |
| `interval_ms` | uint32 | 采样周期（毫秒） |
| `timeout_ms` | uint32 | 单次超时（毫秒） |
| `window_size` | uint32 | 滑动窗口大小（仅 jitter） |
| `bpf_obj` | string | eBPF 对象路径（仅 eBPF 监控器） |

---

### 3.4 `weaknet-cli set <key> <value>`

实时修改监控器参数。**立即生效**，无需重启服务。

```bash
# 修改 RTT 采样周期为 5 秒
$ weaknet-cli set rtt.interval_ms 5s
ok

# 修改 RTT 探测目标
$ weaknet-cli set rtt.target 8.8.8.8
ok

# 启用/禁用监控器
$ weaknet-cli set rtt.enabled false
ok

# 修改 eBPF 对象路径
$ weaknet-cli set dns.bpf_obj /usr/lib/weaknet/dns_monitor.bpf.o
ok
```

**键格式**：`<monitor>.<field>`，例如 `rtt.interval_ms`、`dns.bpf_obj`、`bluetooth.enabled`

**支持的字段与取值范围**：

| 监控器 | 字段 | 类型 | 取值范围/格式 | 说明 |
|---|---|---|---|---|
| `rtt` | `enabled` | bool | `true` / `false` | 启用开关 |
| `rtt` | `target` | string | IPv4 / 域名 | 探测目标 |
| `rtt` | `interval_ms` | duration | `100ms` ~ `600000ms` | 采样周期，支持 `ms`/`s`/`m` 后缀 |
| `rtt` | `timeout_ms` | duration | `100ms` ~ `60000ms` | 单次超时 |
| `jitter` | `window_size` | uint | `2` ~ `1000` | 滑动窗口大小 |
| `rssi`/`tcp_loss`/... | `interval_ms` | duration | `1000ms` ~ `600000ms` | 采样周期 |
| `bluetooth` | `interval_ms` | duration | `1000ms` ~ `60000ms` | 采样周期 |
| `bluetooth` | `bpf_obj` | string | 路径 | Phase2 eBPF 对象路径 |
| `dns`/`wifi_loss`/... | `bpf_obj` | string | 路径 | eBPF 对象路径 |
| `server` | `data_dir` | string | 路径 | 数据目录 |
| `server` | `log_level` | string | `info`/`warning`/`error`/`fatal` | 日志级别 |

**时长格式**：
- 裸数字 = 毫秒（如 `5000` = 5000ms）
- `500ms`、`5s`、`2m`（毫秒/秒/分钟）

**校验规则**：
- 白名单键：仅接受上表定义的键
- 类型校验：bool/int/string/duration
- 区间校验：超出范围拒绝
- 原子提交：校验全通过后一次性写入，失败保持旧值

**返回**：
- 成功：`ok`
- 失败：错误描述（含原因）

---

## 4. 典型使用场景

### 4.1 调整采样频率（不重启）

```bash
# RTT 默认 10s，改为 3 秒快速捕捉抖动
weaknet-cli set rtt.interval 3s

# Jitter 默认 2s，改为 1 秒
weaknet-cli set jitter.interval 1s

# 恢复默认
weaknet-cli set rtt.interval 10s
```

### 4.2 切换探测目标

```bash
# 从阿里云 DNS 切到 Google DNS
weaknet-cli set rtt.target 8.8.8.8

# 切回阿里云
weaknet-cli set rtt.target 223.5.5.5
```

### 4.3 临时禁用某监控器

```bash
# 不需要 DNS 监控，禁用省资源
weaknet-cli set dns.enabled false

# 之后恢复
weaknet-cli set dns.enabled true
```

### 4.4 批量查看当前配置

```bash
# 查看全部
weaknet-cli get all | jq .

# 只看 eBPF 监控器
weaknet-cli get all | jq '.dns, .wifi_loss, .http_latency'
```

### 4.5 脚本化集成示例

```bash
#!/bin/bash
# monitor_tune.sh - 根据时间段动态调整采样频率

HOUR=$(date +%H)
if (( HOUR >= 9 && HOUR <= 18 )); then
    # 工作时间：高频采样
    weaknet-cli set rtt.interval 2s
    weaknet-cli set jitter.interval 1s
else
    # 非工作时间：低频省电
    weaknet-cli set rtt.interval 30s
    weaknet-cli set jitter.interval 10s
fi
```

---

## 5. 配置文件 vs 实时调参

| 维度 | `/etc/weaknet/config.yaml` | `weaknet-cli set` |
|---|---|---|
| **生效时机** | 服务启动时读取 | 运行时立即生效 |
| **持久性** | 永久（重启保留） | 临时（重启丢失，回落配置文件） |
| **用途** | 基线配置、版本管理、部署标准化 | 临时调优、故障排查、临时实验 |
| **优先级** | 低（启动基线） | 高（运行时覆盖） |

**最佳实践**：
1. 在 `/etc/weaknet/config.yaml` 定义标准基线
2. 运维时用 `weaknet-cli set` 临时调整
3. 验证无误后，同步更新配置文件
4. 下次重启自动生效新基线

---

## 6. 错误处理与常见问题

### 6.1 常见错误

| 错误现象 | 原因 | 解决 |
|---|---|---|
| `客户端未连接` | 服务未运行或 D-Bus 不通 | `systemctl status weaknet-server` 确认服务运行 |
| `空的 key 或 value` | 参数为空 | 检查参数拼写 |
| `unknown monitor: xxx` | 监控器名拼写错误 | `weaknet-cli list` 确认可用名 |
| `rtt.target: invalid IPv4` | IP 格式错误 | 使用合法 IPv4 或域名 |
| `rtt.interval: must be 100ms~600000ms` | 超出范围 | 设置 100ms~600000ms 之间的值 |

### 6.2 开发机调试（连开发板 D-Bus）

```bash
# 开发机通过 SSH 隧道连接开发板 D-Bus
ssh -L 9999:localhost:9999 radxa@192.168.137.210 \
  'sudo dbus-daemon --system --address=unix:path=/run/dbus/system_bus_socket --nofork --print-address' &

# 本地 weaknet_cli 指向隧道
DBUS_SYSTEM_BUS_ADDRESS=unix:path=/tmp/dbus-tunnel weaknet-cli get rtt
```

---

## 7. API 参考（C/C++ 集成）

若需在自有程序中集成，使用 `libweaknet.so` 提供的 C API：

```c
#include "weaknet_client.h"

// 初始化
if (!weaknet_init()) { /* 失败处理 */ }

// 设置参数
char err[256];
weaknet_set_monitor_param("rtt.interval", "5s", err, sizeof(err));

// 查询参数
char buf[8192];
weaknet_get_monitor_param("rtt", buf, sizeof(buf), err, sizeof(err));
printf("%s\n", buf);  // JSON 字符串

// 清理
weaknet_cleanup();
```

**编译链接**：
```bash
gcc -o myapp myapp.c -L/path/to/dist-arm64/client/lib -lweaknet -ldbus-1
```

---

## 8. 版本历史

| 版本 | 日期 | 变更 |
|---|---|---|
| v1.0 | 2026-09-01 | 首个版本：get/set/list、白名单校验、D-Bus 通信 |

---

## 9. 相关文档

- [CLAUDE.md](../CLAUDE.md) - 项目完整操作指南
- [交叉编译与开发板部署.md](../docs/交叉编译与开发板部署.md) - ARM64 编译部署全流程
- [架构设计.md](../docs/架构设计.md) - 系统架构与监控器设计
- [学习路线图.md](../docs/学习路线图.md) - 6.2.1 配置化驱动章节

---

> **提示**：本文档随代码同步更新。如发现命令行为与文档不符，请以 `weaknet-cli --help`（如实现）或源码 `client/weaknet_cli.cpp` 为准。