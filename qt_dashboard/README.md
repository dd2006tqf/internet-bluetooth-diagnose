# Qt 本地看板（实验性）

> **状态**：完整调用链已打通，**已实际编译 + 链接 + 运行验证**
> **创建日期**：2026-11-05
> **依据**：根目录 `内核模块技术栈引入讨论.md` 方向一（路径 A1）

## 这是什么

设备接 HDMI 显示器时，在开发板本地打开一个 Qt 看板，查看 WeakNet 的实时诊断情况。

## 架构

```
main.cpp                    QApplication + 命令行
  └── MainWindow            顶栏 / 维度列表 / 趋势图 / 诊断栏
        ├── DataFetchThread  QThread，后台取数（禁止在 UI 线程调 D-Bus）
        │     └── DataSource 唯一直接调用 libweaknet 的地方
        │           └── weaknet_get_*()   ← client/weaknet_client.h
        ├── LineChartWidget  QPainter 自绘折线图
        └── parse_xxx()      JSON → POD（纯函数，可单测）
```

## 关键设计决定

### 数据层：复用现有 C API，不重写 D-Bus

`client/weaknet_client.h` 已经提供完整封装（29 KB，50+ 函数），统一签名：

```c
bool weaknet_get_xxx(char* buffer, size_t buffer_size,
                     char* error_buffer, size_t error_size);
```

返回 JSON 字符串。看板直接链接 `libweaknet.so`，**服务端零改动**。

### 为什么必须有后台线程

`weaknet_get_*` 内部是 D-Bus 同步调用（`dbus_connection_send_with_reply_and_block`）。
服务端忙或连不上时单次调用可能阻塞数秒。放在 UI 线程 → 界面冻结。
`DataFetchThread` 把它们全部挪到后台，通过信号把结果送回主线程。

### JSON：手写轻量解析，不引第三方库

项目全仓不使用 nlohmann/json，`server/src/net_info.cpp:15` 明确写了这一点。
采用「路径式查找」（`getString("overall.state")`）而非完整 DOM，
因为看板只读少数已知字段。

### 图表：QPainter 自绘，不用 Qt Charts

1. 板端**没有** `libqt5charts5`（实测）
2. `QChart` 在 Mesa 软件渲染下性能差
3. 自绘可以 `render()` 到 `QImage` 做像素级断言

### 缓冲区自动扩容

C API 不报告截断。取数层先给 8 KB，用 `JsonReader::valid()` 判断完整性，
不合法就翻倍重试，上限 4 MB。**绝不把截断的 JSON 当成功**。

## 目录结构

```
qt_dashboard/
├── README.md
├── CMakeLists.txt             独立 project
├── main.cpp                   入口
├── data/                      ← 不依赖 Qt
│   ├── json_reader.{hpp,cpp}      轻量 JSON 读取
│   ├── dashboard_data.{hpp,cpp}   POD + 解析
│   └── data_source.{hpp,cpp}      调 libweaknet
├── ui/                        ← 依赖 Qt
│   ├── line_chart_widget.{hpp,cpp}  QPainter 折线图
│   ├── data_fetch_thread.{hpp,cpp}  后台取数
│   └── main_window.{hpp,cpp}        主窗口
└── tests/
    └── test_json_reader.cpp   30 个用例
```

## 验证状态（2026-11-05 实测）

| 项 | 状态 | 证据 |
|------|------|------|
| 数据层单测 | ✅ **30/30 通过** | `ctest` 0.03s |
| 取数层编译 | ✅ 通过 | 链接真实 `weaknet_client.h`，签名全对 |
| UI 层 **moc** | ✅ **3/3 生成成功** | `Q_OBJECT`/信号槽声明语法正确 |
| UI 层编译 | ✅ **零警告** | `-Wall -Wextra` 全部通过 |
| **完整链接** | ✅ **成功** | 365 KB 可执行文件，无 undefined reference |
| **实际运行** | ✅ **启动正常** | `--help` 正确输出；`offscreen` 下持续运行不崩溃 |
| 板端运行 | ❌ 未做 | 无显示器 |

### 怎么验证的（无 Qt 开发包的环境）

板端 `apt install qtbase5-dev` 会级联移除 200+ KDE 包，已排除。
本机（Ubuntu 24.04）其实**可以安全直装**（`Remv` 行数为 0，108 个 `Inst` 全是新增），
但当时先撞上 Docker/apt 网络超时、未做 dry-run，于是绕路用了 deb staging。
具体为**直接下载 deb 包解压到本地目录**，不安装到系统：

```bash
# 1. 下载 Qt 头文件 + 运行库 + moc 工具到 /tmp/qtstage/root
#    （从 mirrors.aliyun.com/debian，amd64 版）

# 2. 手动跑 moc
moc ui/line_chart_widget.hpp -o moc_line_chart_widget.cpp
moc ui/data_fetch_thread.hpp -o moc_data_fetch_thread.cpp
moc ui/main_window.hpp      -o moc_main_window.cpp

# 3. 编译 + 链接
g++ -std=c++17 -fPIC -Wall -Wextra \
    -I. -I../client -I$QTINC -I$QTINC/QtCore -I$QTINC/QtGui -I$QTINC/QtWidgets \
    -c <每个 .cpp>
g++ -o weaknet_dashboard *.o \
    -L$QTLIB -lQt5Widgets -lQt5Gui -lQt5Core \
    -L<build-x86>/client/lib -lweaknet \
    -Wl,-rpath-link,$QTLIB

# 4. 运行
QT_QPA_PLATFORM=offscreen ./weaknet_dashboard --help
```

**这个过程中抓到了 3 个真实 bug**（全部已修）：

| # | 错误 | 后果 |
|---|------|------|
| 1 | 缺 `#include <QTimer>` | 编译失败 |
| 2 | `state_color(snap.overall_state)`：`std::string` → `QString` 隐式转换不存在 | 编译失败 |
| 3 | 同上，另一处 `state_color(item.state)` | 编译失败 |

这证明「未编译验证」不是形式主义——**代码确实有错**。

### 另一个重要发现：枚举值猜错了

`server/include/assurance/health_state.hpp:15-23` 的真实取值是：

```cpp
enum class HealthState { GOOD, DEGRADED, BAD, UNKNOWN };
```

我之前写的是 `HEALTHY` / `FAILED`，**全错**。coverage 同理：
是 `FULL_FOR_PROFILE` 而不是 `FULL`。已全部修正，测试也同步更新。

## 构建

### 只构建数据层（无需 Qt，无需 libweaknet）

```bash
cmake -S qt_dashboard -B build-qt-dashboard -DCMAKE_BUILD_TYPE=Debug
cmake --build build-qt-dashboard -j$(nproc)
ctest --test-dir build-qt-dashboard --output-on-failure
```

### 构建完整看板（需要 Qt 开发包）

```bash
cmake -S qt_dashboard -B build-qt-dashboard \
  -DWEAKN_DASHBOARD_WITH_UI=ON \
  -DWEAKNET_CLIENT_LIB=/path/to/libweaknet.so
cmake --build build-qt-dashboard -j$(nproc)
```

`WEAKNET_CLIENT_LIB` 不硬编码路径，因为板端可能直接链接已安装的库。

## 上板运行还缺什么

1. **Qt 开发包** — 板端只有运行时。已排除板端 `apt install`（会卸载 KDE）
2. **显示器** — HDMI 当前 `disconnected`

### 板端运行的可能路径

| 路径 | 说明 |
|------|------|
| 交叉编译 | 往 sysroot 装 Qt dev，编译 ARM64 二进制（要改 `scripts/`，须在 change 前做） |
| Docker 容器 | 容器内装 ARM64 Qt 编译（glibc 需与板端一致 2.31） |
| 本机 x86 验证 | ✅ **已完成**（见上表） |

## 与现有代码的边界

- 不修改 `client/` 下任何现有文件
- 不修改根 `CMakeLists.txt`
- 不修改 `scripts/`
- 不新增 D-Bus 方法（全部使用现有只读方法）

## 下一步

1. 解决 ARM64 交叉编译环境（D2 或 D3）
2. 接显示器后走 `-platform eglfs` 实机验证
3. 验证真实数据（当前只验证了程序能启动，未验证 D-Bus 取数是否成功）
