# AI-powered Network Diagnostics

一个基于 eBPF 和 D-Bus 的实时网络监控系统，提供网络接口状态监控、流量分析、网络质量评估、蓝牙设备监控等功能。

## 🚀 快速开始

### 自动安装

```bash
# 安装依赖并编译
./install.sh

# 仅安装系统依赖
./install.sh --install-deps
```

### 手动安装

```bash
# 1. 安装依赖 (Ubuntu/Debian)
sudo apt-get update
sudo apt-get install -y build-essential clang llvm pkg-config libdbus-1-dev libglog-dev libelf-dev zlib1g-dev libcap-dev linux-headers-$(uname -r) libbpf-dev

# 2. 编译项目
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_EBPF=OFF
cmake --build build -j$(nproc)

# 3. 启动服务器
./build/server/weaknet-dbus-server

# 4. 运行测试 (新终端)
./test-client.sh
```

## 📁 项目结构

```
AI-powered-Network-Diagnostics/
├── server/                 # 服务器端
│   ├── include/           # 头文件
│   ├── src/               # 源代码
│   ├── test/              # 单元测试
│   └── CMakeLists.txt     # 服务器构建配置
├── client/                # 客户端
│   ├── lib/               # 动态库
│   ├── bin/               # 测试程序
│   ├── client.cpp         # 客户端源码
│   ├── weaknet_client.h   # C API 接口
│   └── CMakeLists.txt     # 客户端构建配置
├── CMakeLists.txt         # 根构建配置
├── tools/                 # 工具脚本
│   ├── ci.sh              # CI/部署脚本
│   └── weaknet-test-full.sh  # 开发板测试脚本
├── docs/                  # 项目文档
├── openspec/              # OpenSpec 变更管理
└── README.md              # 项目说明
```

## 🔧 功能特性

### 服务器端功能
- **网络接口监控**: 实时监控网络接口状态变化
- **RTT监控**: 基于ping的网络延迟监控
- **RSSI监控**: Wi-Fi信号强度监控
- **TCP丢包率监控**: 基于eBPF的内核级丢包监控
- **流量分析**: 基于eBPF的实时流量分析
- **网络质量评估**: 综合多指标的网络质量评估
- **蓝牙监控**: 蓝牙设备发现、连接状态、信号强度监控
- **事件系统**: 基于D-Bus的事件通知机制

### 客户端功能
- **C/C++ API**: 提供完整的C和C++接口
- **动态库**: 可链接的动态库 `libweaknet.so`
- **命令行工具**: 丰富的命令行测试工具
- **事件订阅**: 支持多种网络事件订阅
- **健康检查**: 网络健康状态检查

## 📖 使用方法

### 启动服务器

```bash
# 方式1: 使用启动脚本
./start-server.sh

# 方式2: 直接启动
./server/bin/weaknet-dbus-server

# 方式3: 使用CMake
cmake --build build --target weaknet-dbus-server && ./build/server/weaknet-dbus-server
```

### 客户端测试

```bash
# 运行所有测试
./test-client.sh all

# 获取网络接口信息
./test-client.sh get

# 网络健康检查
./test-client.sh health

# 事件监听测试
./test-client.sh events

# Ping测试
./test-client.sh ping google.com
```

### C/C++ 编程接口

```cpp
#include "client/weaknet_client.h"

// 初始化
if (!weaknet_init()) {
    std::cerr << "初始化失败" << std::endl;
    return -1;
}

// 获取网络接口信息
char buffer[1024], error_buffer[256];
if (weaknet_get_interfaces(buffer, sizeof(buffer), error_buffer, sizeof(error_buffer))) {
    std::cout << "网络接口: " << buffer << std::endl;
}

// 清理
weaknet_cleanup();
```

## 🛠️ 编译选项

```bash
# 编译所有组件
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_EBPF=OFF
cmake --build build -j$(nproc)

# 仅编译服务器
cmake --build build --target weaknet-dbus-server

# 仅编译客户端
cmake --build build --target weaknet test_client_bin

# 清理编译产物
rm -rf build

# 运行测试
ctest --test-dir build/server
```

## 📊 监控指标

### 网络接口指标
- 接口名称和状态
- IP地址和子网掩码
- 网络标志位
- 当前使用状态

### 网络质量指标
- RTT (往返时间)
- TCP丢包率
- RSSI (信号强度)
- 流量统计
- 综合质量评分

### 事件类型
- `InterfaceChanged`: 网络接口变化
- `ConnectionModeChanged`: 上网方式变化
- `NetworkQualityChanged`: 网络质量变化
- `BluetoothDeviceChanged`: 蓝牙设备变化

## 🔍 故障排除

### 常见问题

1. **编译失败**
   ```bash
   # 检查依赖
   ./install.sh --install-deps
   
   # 清理重新编译
   rm -rf build && cmake -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_EBPF=OFF && cmake --build build -j$(nproc)
   ```

2. **服务器启动失败**
   ```bash
   # 检查DBus服务
   systemctl status dbus
   
   # 检查端口占用
   lsof -i :session
   ```

3. **客户端连接失败**
   ```bash
   # 检查服务器是否运行
   pgrep -f weaknet-dbus-server
   
   # 检查DBus连接
   dbus-send --session --dest=com.example.WeakNet --type=method_call --print-reply /com/example/WeakNet com.example.WeakNet.Get
   ```

### 日志文件

- 服务器日志: `./logs/server/`
- 编译日志: 查看终端输出
- 系统日志: `journalctl -f`

## 📚 详细文档

- [架构设计文档](docs/架构设计.md) - 系统架构和技术细节
- [项目评估报告](docs/项目评估.md) - 项目评估和分析
- [学习路线图](docs/学习路线图.md) - 学习路径和扩展方向
- [交叉编译与开发板部署](docs/交叉编译与开发板部署.md) - ARM64 部署指南
- [蓝牙监控优化方案](docs/蓝牙监控优化实现方案.md) - 蓝牙功能优化
- [蓝牙修复方案](docs/蓝牙修复方案.md) - 蓝牙事件路由修复
- [客户端API文档](client/README_CLIENT.md)
- [动态库使用指南](client/README_LIBRARY.md)

## 🤝 贡献

欢迎提交Issue和Pull Request来改进项目。

## 📄 许可证

本项目采用MIT许可证，详见LICENSE文件。

## 🔗 相关链接

- [eBPF官方文档](https://ebpf.io/)
- [DBus官方文档](https://dbus.freedesktop.org/)
- [libbpf项目](https://github.com/libbpf/libbpf)

