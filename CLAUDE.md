# AI-powered Network Diagnostics 项目

基于 D-Bus 的 Linux 网络诊断与弱网监控系统，实时监测网络状态、蓝牙设备、Wi-Fi 信号强度等指标，并通过 D-Bus 将变化通知给客户端。

## 技术栈

- **语言**: C++17
- **IPC**: libdbus-1（D-Bus 会话总线 + 系统总线）
- **蓝牙**: BlueZ D-Bus API（系统总线 org.bluez）
- **eBPF**: TCP 丢包率监控（内核级）
- **Wi-Fi**: wpa_supplicant ctrl_interface（UNIX DGRAM socket）
- **日志**: glog

## 目录结构

```
├── server/                  # 服务端
│   ├── src/                 # 源代码
│   │   ├── server.cpp       # 主入口，启动所有监控线程
│   │   ├── dbus_service.cpp # D-Bus 方法处理与信号发射
│   │   ├── bt_monitor.cpp   # 蓝牙监测器（BlueZ D-Bus）
│   │   ├── event_manager.cpp# 全局事件管理器
│   │   ├── rtt_monitor.cpp  # RTT 延迟监控
│   │   ├── rssi_monitor.cpp # Wi-Fi RSSI 监控
│   │   ├── tcp_loss_monitor.cpp # TCP 丢包率监控
│   │   ├── weak_netmgr.cpp  # 弱网管理器
│   │   ├── net_info.cpp     # 网卡信息模型
│   │   ├── traffic_analyzer.cpp # 流量分析
│   │   └── flow_rate.bpf.c  # eBPF 程序
│   ├── include/             # 头文件
│   ├── build/               # 构建输出（自动生成）
│   │   └── vmlinux.h        # eBPF 头文件（bpftool 自动生成）
│   └── Makefile
├── client/                  # 客户端（动态库）
│   ├── client.cpp           # 客户端实现 + C API
│   ├── weaknet_client.h     # C API 头文件
│   └── Makefile
├── AI-assisted analysis/    # AI 分析模块（Python）
├── docs/                    # 项目文档
├── .gitignore
└── CLAUDE.md                # 本文件
```

## 构建方式

### 服务端

```bash
cd server
make          # 编译
make clean    # 清理
make run      # 运行
```

**注意**: `build/vmlinux.h` 会在编译前通过 `bpftool` 自动生成，需要内核支持 BTF（`/sys/kernel/btf/vmlinux`）。

### 客户端

```bash
cd client
make          # 编译动态库和测试程序
```

## 监控线程一览

| 线程 | 周期 | 输出信号 |
|------|------|---------|
| 网卡监控 | 10s | `InterfaceChanged` |
| 当前上网网卡 | 10s | `ConnectionModeChanged` |
| RTT 延迟 | 10s | `RttChanged` |
| Wi-Fi RSSI | 10s | `RssiChanged` |
| TCP 丢包率 | 10s | `TcpLossRateChanged` |
| 流量分析 | 10s | `Changed` |
| 网络质量 | 15s | `NetworkQualityChanged` |
| 蓝牙监测 | 3s | `BluetoothDeviceChanged` |

## D-Bus 接口

**服务名**: `com.example.WeakNet`
**对象路径**: `/com/example/WeakNet`

### 方法
- `GetInterfaces()` → 返回网卡列表
- `HealthCheck()` → 返回网络健康检查结果
- `Ping(hostname)` → 返回 ping 结果
- `GetBluetoothDevices()` → 返回蓝牙设备列表
- `GetBluetoothAdapter()` → 返回适配器信息

### 信号
- `Changed` — 通用状态变化
- `InterfaceChanged` — 网卡变化
- `ConnectionModeChanged` — 上网方式变化
- `NetworkQualityChanged` — 网络质量变化
- `BluetoothDeviceChanged` — 蓝牙设备变化

## 重要编码规范

### 蓝牙事件路由（关键！）

**所有蓝牙事件必须通过 EventManager 统一分发**：

```cpp
// ✅ 正确：走 EventManager
getEventManager().emitBluetoothDeviceChanged(ev.message, ev.deviceName);

// ❌ 错误：直接调用 emitSpecificSignal
ctx->service->emitSpecificSignal(kSignalInterfaceChanged, ...);

// ❌ 错误：混入 Wi-Fi RSSI 通道
getEventManager().emitRssiChanged(...);
```

蓝牙事件必须发射 `BluetoothDeviceChanged` 信号，不能使用 `InterfaceChanged` 或 `RssiChanged`。

### 线程安全

- 所有共享数据通过 `mutex` 保护
- 使用 `lock_guard<std::mutex>` 自动管理锁
- `atomic<bool>` 用于简单的布尔标志

### Commit Message 格式

```
type: description

- detail 1
- detail 2
```

type 包括：`feat`、`fix`、`chore`、`docs`、`refactor`

## 注意事项

- **不要提交编译产物**（`bin/`, `build/`, `*.o`, `*.so`），已在 `.gitignore` 中排除
- **不要提交 `vmlinux.h`**，由 Makefile 自动生成
- **不要使用 `detach()` 管理新线程**，优先使用线程池或 join
- **新增监控指标时**，同步更新 `EventManager` 和 D-Bus 信号定义
- **修改 D-Bus 接口后**，更新本文档的接口说明

## 依赖

### 系统依赖
- `libdbus-1-dev` — D-Bus 开发库
- `libbpf-dev` — eBPF 开发库
- `libglog-dev` — Google 日志库
- `clang` — eBPF 程序编译
- `bpftool` — 生成 vmlinux.h

### 安装（Ubuntu/Debian）
```bash
sudo apt install libdbus-1-dev libbpf-dev libglog-dev clang linux-tools-generic
```
