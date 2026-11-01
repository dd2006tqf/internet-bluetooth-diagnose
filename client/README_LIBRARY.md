# WeakNet 客户端动态库

WeakNet客户端动态库 (`libweaknet.so`) 提供了一个C接口，供其他应用程序调用WeakNet D-Bus服务的功能。

## 📦 库文件

- **动态库**: `build-x86/client/lib/libweaknet.so`（ARM64 部署包对应 `dist-arm64/client/lib/libweaknet.so`）
- **头文件**: `client/weaknet_client.h`
- **测试程序**: `build-x86/client/bin/test_client_bin`（ARM64 部署包对应 `dist-arm64/client/bin/test-client`）
- **示例程序**: `client/example_usage.cpp`（构建为 `build-x86/client/bin/example_usage`）

## 🚀 快速开始

### 1. 编译动态库

```bash
cd <仓库根目录>
cmake -B build-x86 -DCMAKE_BUILD_TYPE=Debug -DBUILD_EBPF=OFF
cmake --build build-x86 --target weaknet test_client_bin
```

### 2. 基本用法

#### C语言程序示例

```c
#include "weaknet_client.h"

int main() {
    // 初始化
    if (!weaknet_init()) {
        fprintf(stderr, "初始化失败\n");
        return 1;
    }
    
    // 获取网络接口信息
    char buffer[4096], error[256];
    if (weaknet_get_interfaces(buffer, sizeof(buffer), error, sizeof(error))) {
        printf("网络接口: %s\n", buffer);
    }
    
    // Ping指定主机
    if (weaknet_ping_host("google.com", buffer, sizeof(buffer), error, sizeof(error))) {
        printf("Ping结果: %s\n", buffer);
    }
    
    // 清理
    weaknet_cleanup();
    return 0;
}
```

#### C++程序示例

> ⚠️ **事件订阅族已于 2026-11-01 删除**。服务端自 2026-10-19 起停发全部
> D-Bus 信号，`weaknet_subscribe_*` / `weaknet_check_*` 长期"返回成功但永远
> 等不到事件"。它们已被移除（详见 `weaknet_client.h` 文件头）。
>
> **需要持续监测时，按固定周期重复调用对应的 `get_*` / `*_stats` 查询方法：**

```cpp
#include "weaknet_client.h"
#include <iostream>
#include <unistd.h>

int main() {
    weaknet_init();

    char buf[4096], err[256];
    for (int i = 0; i < 60; ++i) {          // 每分钟轮询一次，共 60 次
        if (weaknet_health_check(buf, sizeof(buf), err, sizeof(err))) {
            std::cout << "健康快照: " << buf << std::endl;
        } else {
            std::cerr << "查询失败: " << err << std::endl;
        }
        sleep(1);
    }

    weaknet_cleanup();
    return 0;
}
```

### 3. 编译用户程序

```bash
# 使用动态库编译用户程序
g++ -std=c++17 -I/path/to/client \
    -L/path/to/build-x86/client/lib -lweaknet \
    user_program.cpp -o user_program

# 运行时设置库路径
LD_LIBRARY_PATH=/path/to/build-x86/client/lib:$LD_LIBRARY_PATH \
DBUS_SESSION_BUS_ADDRESS=$DBUS_SESSION_BUS_ADDRESS \
./user_program
```

## 📋 API 参考

### 初始化和清理

```c
bool weaknet_init();                    // 初始化库
void weaknet_cleanup();               // 清理资源
bool weaknet_is_connected();          // 检查连接状态
```

### 信息获取

```c
bool weaknet_get_interfaces(char* buffer, size_t buffer_size, char* error_buffer, size_t error_size);
bool weaknet_health_check(char* result_buffer, size_t result_size, char* error_buffer, size_t error_size);
bool weaknet_ping_host(const char* hostname, char* result_buffer, size_t result_size, char* error_buffer, size_t error_size);
```

### 事件系统 / 状态监控

**已于 2026-11-01 删除。** 服务端停发全部 D-Bus 信号后，以下 8 个 API 长期
处于空转状态（返回成功但永不产生事件），已从 `libweaknet.so` 移除：

```
weaknet_subscribe_event / weaknet_unsubscribe_event
weaknet_subscribe_bluetooth_events / weaknet_subscribe_network_quality
weaknet_check_changes / weaknet_check_events / weaknet_check_network_quality
weaknet_get_event_types
```

改用对应的 `get_*` / `*_stats` 查询方法（见上方「信息获取」）。

## 🧪 测试

```bash
# 测试基本功能（先完成 build-x86 构建）
ctest --test-dir build-x86/server -R 'test_net_info|test_quality|test_serializer'

# 测试事件监听
./build-x86/client/bin/test_client_bin test-events

# 测试特定功能
./build-x86/client/bin/test_client_bin get
./build-x86/client/bin/test_client_bin ping google.com
./build-x86/client/bin/test_client_bin test-basic
./build-x86/client/bin/test_client_bin test-events
```

## 🔧 支持的事件类型

- `InterfaceChanged` - 网络接口变化
- `ConnectionModeChanged` - 连接方式变化  
- `NetworkQualityChanged` - 网络质量变化

## 📝 使用注意事项

1. **初始化顺序**: 必须先调用 `weaknet_init()` 才能使用其他功能
2. **资源清理**: 程序退出前应调用 `weaknet_cleanup()`
3. **错误处理**: 所有函数返回 `false` 时检查错误缓冲区
4. **线程安全**: 库内部使用DBus，基本是线程安全的
5. **环境依赖**: 需要 D-Bus 系统总线运行 WeakNet 服务器（服务端注册于 system bus）

## 🌐 集成示例

### Python绑定示例

```python
import ctypes

# 加载动态库
lib = ctypes.CDLL("./libweaknet.so")

# 定义C接口函数
lib.weaknet_init.argtypes = []
lib.weaknet_init.restype = ctypes.c_bool

lib.weaknet_get_interfaces.argtypes = [ctypes.c_char_p, ctypes.c_size_t, 
                                       ctypes.c_char_p, ctypes.c_size_t]
lib.weaknet_get_interfaces.restype = ctypes.c_bool

# 使用库
if lib.weaknet_init():
    buffer = ctypes.create_string_buffer(4096)
    error = ctypes.create_string_buffer(256)
    if lib.weaknet_get_interfaces(buffer, 4096, error, 256):
        print(f"网络信息: {buffer.value.decode()}")
```

### Node.js绑定示例

```javascript
const ffi = require('ffi-napi');

const lib = ffi.Library('./libweaknet.so', {
    'weaknet_init': ['bool', []],
    'weaknet_get_interfaces': ['bool', ['string', 'size_t', 'string', 'size_t']],
    'weaknet_cleanup': ['void', []]
});

// 使用库
const buffer = Buffer.alloc(4096);
const error = Buffer.alloc(256);

if (lib.weaknet_init()) {
    if (lib.weaknet_get_interfaces(buffer, 4096, error, 256)) {
        console.log('网络信息:', buffer.toString());
    }
    lib.weaknet_cleanup();
}
```

这样其他的应用程序就可以通过这个动态库轻松集成WeakNet的弱网监控和事件监听功能了！
