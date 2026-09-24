# AI-powered Network Diagnostics (WeakNet)

本项目由“tanqf”开发。

一个面向 Linux 嵌入式设备（Radxa Cubie A7A ARM64）与通用服务器的高性能、内核态无感知实时网络可观测与诊断系统。系统以 eBPF（CO-RE）为底层无侵入探针，结合系统总线 D-Bus IPC、多源指标注册中心、分层服务水平评价体系（SLE），以及与大模型智能排障平台（Large-Model-Application）的边缘遥测导出闭环，实现全方位的网络质量监测、弱网诊断与根因分析。

---

## 🌟 核心特性与架构亮点

### 1. 内核级单源可观测底座（eBPF CO-RE）
- **TCP 连接状态机跟踪** (`tcp_connect`, `tcp_conn`): 挂载 `sock:inet_sock_set_state` 等跟踪点，微秒级捕获握手耗时、三次握手丢包与连接生命周期。
- **TCP 协议栈异常诊断** (`tcp_retrans`, `skb_drop`): 精准监控内核快速重传、超时重传（RTO），并在 `kfree_skb` 处捕获丢包 drop reason 代码。
- **应用层协议延迟分析** (`http_latency`, `dns_monitor`): 基于 `sys_enter_write/sendto` 等系统调用与套接字探针，无侵入重组 HTTP 请求往返时延（RTT）及 DNS 解析耗时。
- **单一事实源与跨探针安全**：严格遵循单源捕获原则，严禁跨探针 join 与脆弱内存跨域推断；支持针对本地自采样流量的智能回环抑制与协议过滤。

### 2. 丰富的主被动多源指标监控（Metrics Registry）
- **网络质量与链路监控**：
  - **RTT & Jitter**: 集成 ICMP 探测线程，单次采样下基于滑动窗口实时推导抖动（Jitter），杜绝双倍 ICMP 探测开销。
  - **Wi-Fi 射频态势**: 优先通过 Netlink `nl80211` 查询真实 RSSI，在无射频接口时回落至启发式评估，并支持 `wifi_loss` 帧级监测。
  - **网络流量吞吐**: 实时统计网卡吞吐率（bps）、包速率（pps）及活动流数量。
  - **蓝牙设备监控**: 基于 BlueZ D-Bus 接口异步监测蓝牙适配器、设备配对、连接状态与 RSSI 变化。
- **配置化与热调参（weaknet-cli）**:
  - 全局参数支持 YAML 持久化配置与运行时 D-Bus 动态调参，改动实时生效无需重启服务。
  - 安全鉴权保护：调参及监控器启停接口强校验调用方权限（仅 UID 0 / root 授权修改）。

### 3. 分层网络体验评价体系（Assurance v2 / SLE）
- **Profile 驱动**：根据业务场景（`NETWORK_ONLY` 纯局域网 / `INTERNET_ACCESS` 互联网接入）动态调整评估标准。
- **Evidence-First 证据判决**：多维度置信度门禁，防止弱网误报；区分链路物理质量（Link Quality）与端到端网络体验（Overall Quality）。
- **历史数据持久化**：SQLite 高效持久化网卡健康、RTT、Jitter、丢包率等全维度数据，并配备专用查询 CLI。

### 4. 边缘遥测与 AI 智能诊断平台闭环
- **边缘安全上报**：板端内置轻量签名与导出管道，通过 Ed25519/HMAC 签名与 TLS 双向加密，向云端安全上送不可变评估快照。
- **云端大模型工作台**（`Large-Model-Application`）：基于 Next.js + FastAPI + 大语言模型，接收边缘上报数据，提供暗黑风态势大屏、拓扑下钻以及一键式 AI 根因诊断。

---

## 📁 目录结构

```
AI-powered-Network-Diagnostics/
├── server/                     # C++ 服务端与 eBPF 探针源码
│   ├── src/                    # 服务端实现（WeakNetMgr, D-Bus Adapter, Registry 等）
│   ├── include/                # 服务端内部头文件
│   ├── bpf/                    # eBPF 内核态源码（*.bpf.c 及 vmlinux.h）
│   ├── test/                   # 单元测试与组件测试（Google Test）
│   └── CMakeLists.txt          # 服务端构建规则
├── client/                     # 客户端库与 CLI 工具
│   ├── client.cpp              # 客户端核心实现与 D-Bus 通信
│   ├── weaknet_cli.cpp         # 运维调参命令行工具（weaknet-cli）
│   ├── weaknet_client.h        # 纯 C 兼容 API 头文件
│   └── CMakeLists.txt          # 客户端编译规则
├── Large-Model-Application/    # 智能诊断云端平台（Next.js + FastAPI + LLM Agent）
├── tools/                      # 部署、测试与运维工具
│   ├── ci.sh                   # 一键 CI 脚本：增量编译 + 打包 + 部署 + 真机测试（唯一入口）
│   ├── weaknet-server.service  # systemd 服务单元
│   ├── com.example.WeakNet.conf# D-Bus 系统总线安全策略
│   └── weaknet-test-full.sh    # 开发板真机全量冒烟测试脚本
├── docs/                       # 项目全景技术文档
│   ├── ai/                     # OpenSpec 工作流、铁律与规范
│   ├── 架构设计.md             # 系统全局架构与模块设计
│   ├── 网络体验评价体系.md      # 分层 SLE 评估体系模型
│   ├── 交叉编译与开发板部署.md  # ARM64 开发板环境与编译规范
│   └── weaknet_cli_usage.md    # 调参 CLI 使用手册
├── config.yaml                 # 运行时监控器与边缘上报配置文件
├── CMakeLists.txt              # 根 CMake 配置
└── README.md                   # 项目介绍文档
```

---

## 🚀 快速上手

### 环境依赖

- **开发宿主机（x86_64）**：Linux 环境，支持 Docker、QEMU binfmt（用于 ARM64 模拟构建）、CMake 3.16+、Clang/LLVM、D-Bus、Glog、SQLite3、GTest。
- **目标设备（ARM64）**：Radxa Cubie A7A（内核 5.15+ 支持 BTF 与 eBPF，开启 mDNS / Avahi，预装 libdbus、libbpf、glibc 2.31+）。

### 1. 本地快速构建与测试（x86 验证）

仅需本地开发机快速跑单元测试或校验服务端逻辑时：

```bash
# 生成 x86 构建目录（跳过需要内核环境的 eBPF 探针）
cmake -B build-x86 -DCMAKE_BUILD_TYPE=Debug -DBUILD_EBPF=OFF

# 编译全部组件
cmake --build build-x86 -j$(nproc)

# 运行本地全量单元测试（40 个用例全部通过）
ctest --test-dir build-x86/server --output-on-failure
```

### 2. ARM64 交叉编译与一键部署到开发板

本项目目标运行平台为 ARM64。为保证内核头文件（`vmlinux.h`）与 glibc 二进制兼容性，**严禁在 x86 宿主机直接交叉编译**，统一使用常驻 Docker 容器 `weaknet-arm64-dev` 进行增量编译。

#### （1）确保 ARM64 容器正常运行
```bash
# 若宿主重启过，先重置 binfmt 解释器注册并启动容器
docker run --rm --privileged multiarch/qemu-user-static --reset -p yes
docker start weaknet-arm64-dev

# 验证容器环境
docker exec weaknet-arm64-dev uname -m   # 应输出: aarch64
```

#### （2）一键流水线（编译 + 打包 + 部署 + 真机测试）
```bash
# 自动通过 SSH 部署到开发板（优先通过 mDNS 解析或指定 BOARD 环境变量）
BOARD=radxa@radxa-cubie-a7a.local ./tools/ci.sh

# 若处于跨网段环境，直接指定开发板实际 IP：
BOARD=radxa@192.168.2.77 ./tools/ci.sh
```

流水线会自动完成：
- 容器内 ARM64 增量编译与 BPF 探针编译
- 打包输出 `dist-arm64/`
- rsync 增量同步产物至板端 `/home/radxa/weaknet/`（自动保留板端持久化数据库与密钥）
- 配置 systemd 服务与 D-Bus 系统总线并热重启
- 远程执行板端真机冒烟套件（`health`, `get`, `test-basic`, `test-network`, `test-ping`）

---

## 💻 运维与管理（weaknet-cli）

开发板服务端部署成功后，系统通过 D-Bus System Bus 监听请求。

### 查看配置
```bash
# 列出所有可用监控器
weaknet-cli list

# 查询指定监控器当前参数（JSON 输出）
weaknet-cli get rtt
# 输出示例: {"rtt":{"enabled":true,"target":"223.5.5.5","interval_ms":5000,"timeout_ms":800,"window_size":30}}

# 查看全部运行中配置
weaknet-cli get all
```

### 实时调参（需 root 权限）
为保证网络服务安全，修改系统状态的方法已收紧至仅 root 允许：
```bash
# 实时修改 RTT 采样周期为 5 秒
sudo weaknet-cli set rtt.interval 5s

# 修改 RTT 探测目标地址
sudo weaknet-cli set rtt.target 8.8.8.8

# 动态启停指定监控器
sudo weaknet-cli disable bluetooth
sudo weaknet-cli enable bluetooth

# 保存当前运行时参数到持久化覆盖配置
sudo weaknet-cli save
```

### 历史数据查询工具
```bash
# 查询数据库整体统计信息
sudo /home/radxa/weaknet/server/bin/history_query_tool --info

# 按网卡及时间窗口导出历史记录
sudo /home/radxa/weaknet/server/bin/history_query_tool --iface wlan0 --last 1h
```

---

## 🌐 云端协同：Large-Model-Application 大模型排障

系统解耦了边缘实时监测与云端海量智能分析：
- **开发板（边缘设备）**：充当高可靠不可变事实源，按指定周期采样与评估，生成签名遥测帧推送到后端；
- **云端排障工作台**：启动位于 `Large-Model-Application/` 的全功能诊断套件：

```bash
cd Large-Model-Application

# 1. 启动微服务底座（PostgreSQL / Redis / MinIO / Keycloak / OPA / FastAPI）
./scripts/dev_lite.sh init
./scripts/dev_lite.sh up -d

# 2. 启动前端 SPA 工作台
web/node_modules/.bin/pnpm --dir web dev
# 打开浏览器访问: http://localhost:3000
```

---

## 🔌 C / C++ 客户端二次集成

动态链接库 `libweaknet.so` 与头文件 `weaknet_client.h` 为第三方应用提供了极简的接入方式：

```cpp
#include <iostream>
#include "client/weaknet_client.h"

int main() {
    // 1. 初始化客户端总线连接
    if (!weaknet_init()) {
        std::cerr << "WeakNet 客户端初始化失败" << std::endl;
        return -1;
    }

    // 2. 发起健康评估调用
    char result_buf[2048] = {0};
    if (weaknet_health_check(result_buf, sizeof(result_buf))) {
        std::cout << "当前健康状态快照:\n" << result_buf << std::endl;
    }

    // 3. 释放资源
    weaknet_cleanup();
    return 0;
}
```

---

## 🛠️ 工程规范与 Harness 纪律

本项目遵循严格的 **OpenSpec 规范驱动开发** 与 **Superpowers Skills** 流程：
1. **单一事实源**：以 OpenSpec 变更（`openspec/changes/`）为需求、设计与任务拆解的唯一标准。
2. **严禁在未冻结基线下修改代码**：遵循 `Planner → Generator → Evaluator` 三权分立角色体系，确保行为变更拥有完整的 RED-GREEN 证据链。
3. **真实真机闭环**：代码改动必须经过 x86 单元测试、ARM64 容器增量构建、Radxa 开发板真机部署测试全绿后方可合并。

---

## 📄 许可证与权利声明

- 本项目开发与核心维护者：“tanqf”。
- 开源协议：本项目遵循 [MIT 许可证](LICENSE)。
