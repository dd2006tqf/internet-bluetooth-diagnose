# AI-powered Network Diagnostics 项目架构文档

## 一、系统架构总览

本项目是一个 **基于 D-Bus 的 Linux 网络诊断与弱网监控系统**，采用 C/S 架构：

- **服务端**：常驻后台的监控守护进程，持续采集网络状态
- **客户端**：通信代理，封装 D-Bus 复杂性，提供 C API 供第三方集成
- **AI 分析模块**：基于 RAG 的网络问题智能分析（Python）

```
┌──────────────────────────────────────────────────────────────────────┐
│                        服务端 (守护进程)                               │
│                                                                       │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌────────────┐ │
│  │网卡监控   │ │当前上网  │ │RTT延迟   │ │Wi-Fi    │ │TCP丢包率   │ │
│  │线程(10s) │ │网卡(10s) │ │线程(10s) │ │RSSI线程  │ │线程(10s)   │ │
│  └────┬─────┘ └────┬─────┘ └────┬─────┘ └────┬─────┘ └─────┬──────┘ │
│       │             │             │             │              │       │
│  ┌────┴─────────────┴─────────────┴─────────────┴──────────────┴──┐  │
│  │                  WeakNetMgr (弱网管理器)                         │  │
│  │   NetInfo 接口列表 ──→ 更新 ──→ 线程安全访问 (iface_mutex)       │  │
│  └──────────────────────────────┬─────────────────────────────────┘  │
│                                 │                                    │
│  ┌──────────┐ ┌──────────┐ ┌───┴────────┐ ┌──────────────┐         │
│  │流量分析  │ │网络质量  │ │ 蓝牙监测   │ │EventManager  │         │
│  │线程(10s) │ │评估(15s) │ │ 线程(3s)   │ │(全局单例)    │         │
│  └────┬─────┘ └────┬─────┘ └─────┬──────┘ └──────┬───────┘         │
│       │             │              │               │                 │
│       └─────────────┴──────────────┼───────────────┘                 │
│                                    ▼                                 │
│  ┌─────────────────────────────────────────────────────────────────┐│
│  │                     DbusService                                  ││
│  │   方法: GetInterfaces / HealthCheck / Ping                      ││
│  │         GetBluetoothDevices / GetBluetoothAdapter               ││
│  │   信号: Changed / InterfaceChanged / ConnectionModeChanged      ││
│  │         NetworkQualityChanged / BluetoothDeviceChanged          ││
│  └─────────────────────────────────────────────────────────────────┘│
│                                    │                                 │
└────────────────────────────────────┼─────────────────────────────────┘
                                     │ D-Bus Session Bus
┌────────────────────────────────────┼─────────────────────────────────┐
│                        客户端 (动态库)                                 │
│                                    │                                 │
│  ┌─────────────────────────────────┼──────────────────────────────┐  │
│  │              WeakNetClient       │                              │  │
│  │   方法调用: getInterfaces() ────→│                              │  │
│  │              healthCheck() ─────→│                              │  │
│  │              pingHost() ────────→│                              │  │
│  │              getBluetoothDevices()→                             │  │
│  │   信号订阅: subscribeToEvent() ←─┤                              │  │
│  │              subscribeToBluetoothEvents() ←─┤                  │  │
│  │   非阻塞:   checkForEvents() ←───┤                              │  │
│  └─────────────────────────────────────────────────────────────────┘  │
│                                    │                                 │
│  C API 对外接口 (libweaknet.so):                                       │
│  weaknet_init / weaknet_cleanup / weaknet_get_interfaces               │
│  weaknet_health_check / weaknet_ping_host                             │
│  weaknet_subscribe_event / weaknet_check_events                       │
│  weaknet_get_bluetooth_devices / weaknet_subscribe_bluetooth_events   │
└────────────────────────────────────┼─────────────────────────────────┘
                                     │ C API 链接
┌────────────────────────────────────┼─────────────────────────────────┐
│                    第三方应用程序                                      │
│                                    │                                 │
│  UI 界面 / AI 分析模块 / 告警系统    │                                 │
└────────────────────────────────────────────────────────────────────────┘
```

---

## 二、目录结构

```
项目根目录/
├── Makefile                    ← 根 Makefile（一键编译全部）
├── config.mk                   ← 路径配置
├── install.sh                  ← 安装脚本
├── .gitignore
├── CLAUDE.md                   ← AI 项目说明
│
├── server/                     ← 服务端
│   ├── Makefile
│   ├── src/                    ← 源代码（20 个 .cpp 文件）
│   │   ├── main.cpp            ← 程序入口
│   │   ├── server.cpp          ← 服务端主逻辑，启动所有监控线程
│   │   ├── dbus_service.cpp    ← D-Bus 方法处理与信号发射
│   │   ├── weak_netmgr.cpp     ← 弱网管理器，收集网卡信息
│   │   ├── net_info.cpp        ← 网卡信息模型
│   │   ├── bt_monitor.cpp      ← 蓝牙监测器（BlueZ D-Bus API）
│   │   ├── event_manager.cpp   ← 全局事件管理器
│   │   ├── rtt_monitor.cpp     ← RTT 延迟监控
│   │   ├── rssi_monitor.cpp    ← Wi-Fi RSSI 监控
│   │   ├── net_wifiriss.cpp    ← wpa_supplicant 通信
│   │   ├── tcp_loss_monitor.cpp← TCP 丢包率监控
│   │   ├── net_tcp.cpp         ← eBPF TCP 监控
│   │   ├── traffic_analyzer.cpp← 流量分析
│   │   ├── net_traffic.cpp     ← 流量数据采集
│   │   ├── network_quality_assessor.cpp ← 网络质量评估
│   │   ├── net_ping.cpp        ← Ping 功能
│   │   ├── looper.cpp          ← D-Bus 事件循环
│   │   ├── logger.cpp          ← 日志系统
│   │   ├── serializer.cpp      ← 数据序列化
│   │   └── flow_rate.bpf.c     ← eBPF 程序（内核级）
│   └── include/                ← 头文件（18 个 .hpp/.h 文件）
│
├── client/                     ← 客户端（动态库）
│   ├── client.cpp              ← 客户端实现 + C API
│   ├── weaknet_client.h        ← C API 头文件
│   ├── test_client.cpp         ← 测试程序
│   ├── example_usage.cpp       ← 使用示例
│   ├── ping_example.cpp        ← Ping 示例
│   └── test_network_quality.cpp← 网络质量测试
│
├── AI-assisted analysis/       ← AI 分析模块（Python RAG）
│   ├── simple_rag_analyzer.py
│   ├── true_rag_analyzer.py
│   ├── vector_rag_analyzer.py
│   ├── optimized_network_rag.py
│   ├── interactive_rag.py
│   ├── local_vector_rag_analyzer.py
│   ├── log_capture.py
│   └── network_knowledge_base.py
│
├── docs/                       ← 项目文档
│   ├── BLUETOOTH_FIX_PLAN.md   ← 蓝牙事件路由修复方案
│   ├── PROJECT_EVALUATION.md   ← 项目评估报告
│   └── ARCHITECTURE.md         ← 本文件
│
└── README.md                   ← 项目说明
```

---

## 三、技术栈

| 技术 | 用途 |
|------|------|
| **C++17** | 服务端和客户端核心语言 |
| **libdbus-1** | D-Bus IPC 通信（C API） |
| **BlueZ D-Bus API** | 蓝牙设备监测（系统总线 org.bluez） |
| **eBPF** | TCP 丢包率监控（内核级） |
| **wpa_supplicant ctrl_interface** | Wi-Fi RSSI 获取（UNIX DGRAM socket） |
| **glog** | 日志系统 |
| **Python + RAG** | AI 网络问题分析（可选模块） |

---

## 四、服务端职责

### 核心职责：**数据采集 + 状态监控 + D-Bus 服务导出**

服务端是一个**常驻后台的监控守护进程**，持续采集网络状态，并通过 D-Bus 对外提供查询和订阅服务。

### 1. 网络指标采集

| 监控项 | 采集方式 | 周期 | 输出 |
|--------|---------|------|------|
| **网卡列表** | 读取系统网络接口（ioctl/netlink） | 10s | 接口名称、状态、IP、MTU、流量 |
| **当前上网网卡** | 路由表分析（默认路由检测） | 10s | 正在上网的网卡名称 |
| **RTT 延迟** | ping 223.5.5.5（阿里云 DNS） | 10s | 延迟毫秒数 |
| **Wi-Fi 信号强度** | wpa_supplicant SIGNAL_POLL 命令 | 10s | RSSI dBm 值 |
| **TCP 丢包率** | eBPF 内核程序（tcp_retransmit_skb 探针） | 10s | 丢包率百分比 |
| **流量分析** | 读取 /proc/net/dev + eBPF | 10s | 带宽、活跃流数、包率 |
| **蓝牙设备** | BlueZ D-Bus API（GetManagedObjects） | 3s | 设备列表、连接状态、RSSI |

### 2. 数据聚合与评估

- **WeakNetMgr**：维护所有网卡信息的列表，线程安全访问
- **NetworkQualityAssessor**：综合 RSSI + RTT + TCP 丢包率 + 流量，计算网络质量评分（excellent/good/fair/poor/very_poor）

### 3. D-Bus 服务导出

#### 服务标识

```cpp
服务名:    com.example.WeakNet
对象路径:  /com/example/WeakNet
接口名:    com.example.WeakNet
```

#### 提供的方法

| 方法 | 参数 | 返回值 | 说明 |
|------|------|--------|------|
| `Get` | 无 | 字符串 | 示例方法 |
| `GetInterfaces` | 无 | 字符串数组 | 返回所有网卡列表 |
| `HealthCheck` | 无 | 字符串(JSON) | 网络健康检查结果 |
| `Ping` | 主机名 | 字符串 | Ping 结果（含延迟） |
| `GetBluetoothDevices` | 无 | 字符串数组 | 蓝牙设备列表 |
| `GetBluetoothAdapter` | 无 | 字符串 | 适配器状态信息 |

#### 主动推送的信号

| 信号 | 触发条件 | 参数 |
|------|---------|------|
| `Changed` | 通用状态变化 | message, counter |
| `InterfaceChanged` | 网卡添加/移除 | message, counter |
| `ConnectionModeChanged` | 上网网卡切换 | message, counter |
| `NetworkQualityChanged` | 网络质量变化 | quality, details, counter |
| `BluetoothDeviceChanged` | 蓝牙设备变化 | message, counter |

### 4. 事件管理

**EventManager**（全局单例）负责：
- 注册各类事件的回调函数
- 统一事件分发（invokeCallbacks）
- 通过 DbusService 发射对应的 D-Bus 信号

---

## 五、客户端职责

### 核心职责：**消费服务端数据 + 提供 C API 供第三方集成**

客户端是一个**通信代理**，把 D-Bus 的复杂性封装起来，让其他程序用简单的 C API 就能获取网络诊断数据。

### 1. 方法调用（主动查询）

```cpp
weaknet_get_interfaces()    → 获取网卡列表
weaknet_health_check()      → 获取网络健康检查
weaknet_ping_host(hostname) → 发起 Ping 请求
weaknet_get_bluetooth_devices() → 获取蓝牙设备列表
weaknet_get_bluetooth_adapter() → 获取适配器信息
```

### 2. 信号订阅（被动接收）

```cpp
weaknet_subscribe_event(type, callback)   → 订阅指定事件
weaknet_subscribe_bluetooth_events(cb)    → 订阅蓝牙事件
weaknet_subscribe_network_quality(cb)     → 订阅网络质量事件
```

### 3. 非阻塞检查（轮询模式）

```cpp
weaknet_check_events()          → 检查是否有新事件
weaknet_check_network_quality() → 检查网络质量变化
```

### 4. 动态库导出（C API）

客户端编译为 `libweaknet.so`，第三方应用通过链接此库直接使用所有功能：

```c
#include "weaknet_client.h"

weaknet_init();                              // 初始化
weaknet_get_interfaces(buf, sz, err, errsz); // 获取网卡
weaknet_subscribe_event("InterfaceChanged", callback);
weaknet_cleanup();                           // 清理
```

---

## 六、完整通信流程

### 流程 1：客户端主动查询（方法调用）

```
客户端                                     服务端
  │                                         │
  │  1. weaknet_init()                      │
  │  ──→ dbus_bus_get(DBUS_BUS_SESSION)     │
  │                                         │
  │  2. weaknet_get_interfaces()            │
  │  ──→ dbus_message_new_method_call()     │
  │     (kBusName, kObjectPath,             │
  │      kInterface, kMethodGetInterfaces)  │
  │  ──────────────────────────────────────→│
  │                                         │  DbusService::MessageHandlerStatic
  │                                         │  ──→ handleListInterfaces()
  │                                         │     ──→ WeakNetMgr::namesOf(iface_list)
  │  ←──────────────────────────────────────│  返回字符串数组 [eth0, wlan0, ...]
  │                                         │
  │  3. 解析返回结果                         │
  │     "eth0,wlan0"                        │
```

### 流程 2：服务端主动推送（信号通知）

```
监控线程              EventManager            DbusService            D-Bus              客户端
  │                      │                       │                   │                  │
  │ 网卡检测到变化        │                       │                   │                  │
  │  ──────────────────→ │                       │                   │                  │
  │  emitInterfaceChanged│                       │                   │                  │
  │  ("message","source")│                       │                   │                  │
  │                      │ invokeCallbacks()     │                   │                  │
  │                      │ ──→ 内部回调(日志)     │                   │                  │
  │                      │                       │                   │                  │
  │                      │ emitSpecificSignal()  │                   │                  │
  │                      │ ─────────────────────→│                   │                  │
  │                      │                       │ dbus_message_new  │                  │
  │                      │                       │ _signal()         │                  │
  │                      │                       │ ─────────────────→│                  │
  │                      │                       │                   │ Signal:          │
  │                      │                       │                   │ InterfaceChanged │
  │                      │                       │                   │ ────────────────→│
  │                      │                       │                   │                  │ checkForEvents()
  │                      │                       │                   │                  │ 解析信号
  │                      │                       │                   │                  │ 触发回调
```

### 流程 3：蓝牙完整闭环

```
BlueZ D-Bus           BtMonitor             EventManager          DbusService          客户端
(系统总线)              (3s周期)              (全局单例)            (会话总线)           
  │                      │                      │                    │                   │
  │ GetManagedObjects    │                      │                    │                   │
  │ ←─────────────────── │                      │                    │                   │
  │ 返回适配器/设备列表   │                      │                    │                   │
  │                      │                      │                    │                   │
  │                      │ parseDeviceProperties│                    │                   │
  │                      │ ──→ 新设备发现        │                    │                   │
  │                      │ ──→ DeviceFound 事件  │                    │                   │
  │                      │    存入 pendingEvents │                    │                   │
  │                      │                      │                    │                   │
  │                      │ fetchEvents()        │                    │                   │
  │                      │ ──────────────────→  │                    │                   │
  │                      │                      │ emitBluetooth      │                   │
  │                      │                      │ DeviceChanged()    │                   │
  │                      │                      │ ────────────────→  │                   │
  │                      │                      │                    │ emitSpecific      │
  │                      │                      │                    │ Signal()          │
  │                      │                      │                    │ ────────────────→│
  │                      │                      │                    │                   │
  │                      │                      │                    │                   │ 客户端已订阅
  │                      │                      │                    │                   │ BluetoothDeviceChanged
  │                      │                      │                    │                   │ 收到设备发现通知 ✅
```

### 流程 4：网络质量综合评估

```
RTT监控 ───→ rttMs ──────┐
                          │
RSSI监控 ──→ rssiDbm ─────┤
                          │         NetworkQualityAssessor
TCP丢包 ───→ tcpLoss ────→┤              │
                          │     assessQuality()
流量分析 ──→ traffic ─────┤         ──────→ calculateRssiScore()
                          │         ──────→ calculateRttScore()
网卡状态 ──→ usingNow ───→┤         ──────→ calculateTcpLossScore()
                          │         ──────→ calculateTrafficScore()
                          │         ──────→ 综合评分(加权)
                          │                    │
                          │                    ▼
                          │            NetworkQualityResult
                          │            (excellent/good/fair/poor)
                          │                    │
                          │                    ▼
                          │           emitNetworkQualityChanged()
                          │                    │
                          ▼                    ▼
                     EventManager ──→ 客户端 NetworkQuality 回调
```

---

## 七、关键设计决策

### 1. 为什么使用 D-Bus？

- **Linux 原生 IPC**：无需额外依赖，系统自带
- **支持信号推送**：服务端可以主动通知客户端，无需轮询
- **多客户端支持**：多个客户端可以同时订阅同一个信号
- **语言无关**：任何支持 D-Bus 的语言都可以接入

### 2. 为什么蓝牙用系统总线？

- BlueZ（Linux 蓝牙协议栈）运行在系统总线上
- 系统总线需要 root 权限，但蓝牙监测线程通过独立的 D-Bus 连接操作
- 服务端本身运行在会话总线上，两者互不干扰

### 3. 为什么 Wi-Fi RSSI 不走 D-Bus？

- wpa_supplicant 提供的是 UNIX DGRAM socket（ctrl_interface）
- 直接通信比 D-Bus 更高效
- 这是 Linux Wi-Fi 管理的标准方式

### 4. 为什么使用 eBPF 监控 TCP 丢包？

- 内核级探针（tcp_retransmit_skb）可以精确捕获重传事件
- 零开销（仅在内核触发时执行）
- 比用户态抓包更准确

---

## 八、线程安全设计

```
WeakNetMgr::iface_list (共享数据)
         │
    iface_mutex_ (std::mutex)
         │
  ┌──────┼──────────────────────┐
  │      │                      │
写:      │                     读:
网卡线程  │                  DbusService
上网线程  │                  handleListInterfaces()
流量线程  │                  handleHealthCheck()
蓝牙线程  │                  handleGetBluetoothDevices()
  │      │                      │
  └──────┼──────────────────────┘
         │
   lock_guard<std::mutex>
```

所有监控线程写入共享数据时都通过 `lock_guard<std::mutex>` 保护，DbusService 读取时也加相同的锁。

---

## 九、构建与运行

### 编译

```bash
make                    # 一键编译全部（服务端 + 客户端）
cd server && make       # 只编译服务端
cd client && make       # 只编译客户端
make clean              # 清理所有编译产物
```

### 运行服务端

```bash
sudo make run-server    # 需要 root 权限（eBPF）
```

### 运行客户端测试

```bash
make test-client COMMAND=all          # 测试所有功能
make test-client COMMAND=ping baidu.com # Ping 测试
make test-client COMMAND=events       # 事件监听
```

### 系统依赖

```bash
sudo apt install libdbus-1-dev libbpf-dev libglog-dev clang linux-tools-generic
```

---

## 十、重要注意事项

1. **蓝牙事件路由**：所有蓝牙事件必须通过 `EventManager::emitBluetoothDeviceChanged()` 统一分发，不能直接调用 `emitSpecificSignal()`
2. **信号隔离**：蓝牙事件使用 `BluetoothDeviceChanged` 信号，Wi-Fi RSSI 使用 `RssiChanged` 信号，两者不能混淆
3. **编译产物不提交**：`bin/`, `build/`, `*.o`, `*.so`, `vmlinux.h` 等已在 `.gitignore` 中排除
4. **vmlinux.h 自动生成**：编译前通过 `bpftool` 从 `/sys/kernel/btf/vmlinux` 自动生成
