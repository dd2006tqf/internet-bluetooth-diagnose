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
    ├── test_json_reader.cpp        39 个用例（数据层）
    └── test_line_chart_widget.cpp   8 个用例（UI 像素级）
```

## 验证状态（2026-11-05 实测，2026-11-06 续验）

| 项 | 状态 | 证据 |
|------|------|------|
| 数据层单测 | ✅ **39/39 通过** | `ctest` 0.02s，含 6 个真机样本用例 |
| UI 像素级测试 | ✅ **8/8 通过** | `QImage` 渲染断言，含对照组与变异验证 |
| 取数层编译 | ✅ 通过 | 链接真实 `weaknet_client.h`，签名全对 |
| UI 层 **moc** | ✅ **3/3 生成成功** | `Q_OBJECT`/信号槽声明语法正确 |
| UI 层编译 | ✅ **零警告** | `-Wall -Wextra` 全部通过（12/12 编译单元） |
| **完整链接** | ✅ **成功** | 378 KB（386856 B）可执行文件，无 undefined reference |
| **x86 实际运行** | ✅ **启动正常** | `--help` 正确输出；`offscreen` 下持续运行不崩溃 |
| **ARM64 交叉编译** | ✅ **成功** | `tools/build-arm64.sh`，最高 glibc 需求 `2.17`（板端 `2.31`） |
| **初始失败可恢复** | ✅ **实测重试循环** | 指向不存在的 D-Bus socket → 每 2s 重试一次并 `weaknet_cleanup()`，线程不退出、界面仍可截图 |
| **默认平台插件** | ✅ **三场景实测** | 无变量→eglfs；`--screenshot`→offscreen；已设变量不覆盖 |
| **板端真实运行** | ✅ **成功** | arm64 二进制在 `radxa-cubie-a7a` 上启动，D-Bus 连接建立 |
| **真实取数** | ✅ **3/3 成功** | experience 4129B / diagnosis 285B / history 50KB（interfaces/health 已随孤儿清理删除） |
| **真实渲染** | ✅ **像素级验证** | offscreen 截图 1024x742，折线横跨绘图区，状态色齐全 |
| **新控件真渲染到屏** | ✅ **逐项对账** | 顶栏色块 `绿●8 琥珀●1 红●0 灰●4` 与 `busctl GetNetworkExperience` 的 13 维度状态完全一致 |
| **legacy 断开连线** | ✅ **自动化测试 + 变异验证** | 8 条像素测试，破坏 legacy 逻辑后 3 条立即可靠失败 |
| **VNC 图形栈** | ✅ **外部取帧验证** | `libqvnc.so` 跑通，RFB 客户端读到真实帧缓冲 1024x700/32bpp |
| **15 分钟长稳** | ✅ **零泄漏零错误** | RSS 稳定 60300 kB 不变，fd/线程恒定，日志 0 错 |
| HDMI 实机显示 | ⏳ 待做 | 显示器未在位（`HDMI disconnected`） |

### 2026-11-06 续：孤儿清理与真实消费者

上一轮遗留两类问题，已按 CLAUDE.md 的 orphan 铁律处理完毕：

**A 类 —— 删除无消费者的接口**

`LineChartWidget::clear/pointCount/setLineColor/setAutoScale`、`JsonReader::rootIsArray/rootArraySize/raw`、
`EvidenceItem` 结构体、`FetchResult::buffer_used`、`DataSource::initialized()`、
`DiagnosisSnapshot::raw_json`、`HistoryPoint::overall_quality/iface`、
`HistorySeries::interface_name/empty()`。全部零调用，保留即为孤儿。

**B 类 —— 给已解析字段接入真实消费者**

| 字段 | 去向 |
|------|------|
| `count_by_state` | 顶栏彩色 ● 汇总「绿 8 / 黄 1 / 红 0 / 灰 4」 |
| `assessment_profile` + `overall_coverage` | 顶栏「档位 INTERNET_ACCESS / 覆盖 PARTIAL」 |
| `warnings` | 底栏红色警告行（无警告时隐藏、不占版面） |
| `primary_issue` | `summary_template` 为空时兜底成「首要问题 xxx」；两者都空则隐藏 |
| `summary_template` | 底栏独立人读摘要行 |
| `secondary_issues` / `excluded_causes` / `evidence_refs` | 计数上屏 + 明细进 tooltip |
| `actions[].priority` | 诊断行显示「建议动作 N 项（最高优先级 P）」 |
| `SleItem::reason` | 每个维度状态标签的 tooltip |
| `HistoryPoint::legacy` | 折线图断开连线 + 灰色空心点（v1/v2 不同量纲） |
| `schema_version` | 解析守卫：非 0/2 直接标成不可用，避免按 v2 路径静默取空 |

底栏按数据源拆分成 5 个独立 QLabel（experience 2 个、diagnosis 3 个），
每个只有一个写者，任一侧晚到或失败都不会抹掉另一侧已经显示的内容。



### 板端验证是怎么做的（无显示器）

关键在于 `--screenshot` 选项：用 `offscreen` 平台插件跑真实二进制，
等首轮取数完成后把窗口 `grab()` 成 PNG。

```bash
# 板端
LD_LIBRARY_PATH=/home/radxa/weaknet/current/client/lib \
  QT_QPA_PLATFORM=offscreen ./weaknet_dashboard \
  --screenshot /tmp/dash.png --screenshot-delay 5000
```

判据不是"看起来像"，而是可复算的**差分对照**：

| 检查 | 真实数据（wlan0） | 对照（`--interface nosuchif0`） |
|------|------------------|------------------------------|
| `y=341` 折线长度 | **674 px**（横跨绘图区 329..1003） | **无折线** |
| 图表中心提示文字 | **0 px** | **396 px**（"暂无历史数据"） |
| 状态色绿/黄/红/灰 | 75 / 96 / 14 / 428 采样点 | — |

这张图证明折线来自真实 D-Bus 数据，而不是占位符。

### 更强的验证：VNC 外部取帧（2026-11-05 补）

`window.grab()` 是应用自己截图，说服力有限。板端 Qt 自带 `libqvnc.so`，
可以起一个真实 GUI 服务，由**外部 RFB 客户端**读真实帧缓冲：

```bash
ssh board 'cd /tmp && QT_QPA_PLATFORM="vnc:size=1024x700:port=5902" ./weaknet_dashboard'
# 本地用任意 RFB 客户端连过去；或按下面命令直接读像素
```

实测握手与内容：

```text
banner:       RFB 003.003
framebuffer:  1024x700 bpp=32 depth=32 truecolor=1
desktop name: Qt for Embedded Linux VNC Server
折线所在行:   y=344，686 px（水平居中）
中心提示文字: 严格匹配 #999999 → 0 px（有数据）
```

这条路子走的是完整 QPA 图形栈（光栅化、事件分发、GUI 循环），只是输出到
网络而非 HDMI，比 `offscreen` 更接近实机。

### 长稳（15 分钟，2026-11-05）

| 时刻 | RSS | fd | 线程 | CPU | 日志错误 |
|------|-----|----|----|-----|---------|
| 04:12 | 52116 kB | 9 | 3 | 0.9% | 0 |
| 09:56 | 60300 kB | 9 | 3 | 0.8% | 0 |
| 15:19 | 60300 kB | 9 | 3 | 0.8% | 0 |

RSS 抬升到 60300 kB 后连续 10 次采样（每 30 秒）**逐字节不变**，是 VNC/渲染
缓冲的一次性分配，无泄漏。期间约 90 轮 D-Bus 取数无连接失效。

另外做了「删代码前后截图对比」：删除孤立的 `score_range()` 后重跑，
1024x700 中只有 **1042 px** 不同，且全部落在两处文字区域
（右上角时间戳 `y=20..32`、底栏诊断文字 `y=611..622`，都含运行时刻），
图表与维度列表**逐像素一致**，证明重构没有改变渲染行为。

> 这里踩过一个坑：最初的「图表非白像素」判据是假通过——图表背景本身是
> `(239,239,239)`，整片灰底都被算成了内容。改成「折线颜色的连续段」才有效。
> 阈值判据必须锚定到具体颜色与几何位置，不能只看"非白"。

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

### 交叉编译到 ARM64（宿主机直出，不用 Docker/QEMU）

```bash
./qt_dashboard/tools/build-arm64.sh
```

关键技巧：**moc 用宿主机 x86 的，编译器用 arm64 的**。moc 是构建期工具，
产物只是展开 `Q_OBJECT` 宏的普通 C++ 源码，**与目标架构无关**。

前置条件（脚本启动时会一次性检查并说明缺什么）：

1. sysroot 里有 arm64 Qt5 开发文件 → `./qt_dashboard/tools/fetch-qt-arm64-sysroot.sh`
2. 宿主机有 x86 moc → `sudo apt install qtbase5-dev-tools`（零卸载）
3. 已构建 arm64 `libweaknet.so` → `cmake -B build-arm64-native ...`

产物：`build-qt-arm64/weaknet_dashboard`（约 399 KB，glibc 需求仅 2.17）。

## 上板运行还缺什么

**只剩 HDMI 物理链路。** 交叉编译、板端启动、真实取数、离屏渲染、
VNC 图形栈、15 分钟长稳都已实测通过。

接上 HDMI 显示器后，用 `eglfs` 直接跑（需要 `/dev/dri` 权限）：

```bash
ssh board 'sudo QT_QPA_PLATFORM=eglfs /tmp/weaknet_dashboard'
```

### VNC 能验证什么、不能验证什么

已用 `libqvnc.so` 验证图形栈（光栅化、事件分发、GUI 循环）与渲染正确性。
但以下只在物理链路暴露，**必须实机**：

- HDMI 时序 / EDID 协商 / 分辨率与刷新率
- 热插拔（插入、拔出、切换）
- 显存带宽与 eglfs 直出路径
- 板端 `/dev/dri` 权限与 KMS 驱动行为

### 已排除的路径（留作记录，避免重复试）

板端 `apt install qtbase5-dev` 会级联移除 200+ KDE 包，不可用：

```text
$ ssh board 'apt-get -s install qtbase5-dev > /tmp/qt-dryrun.txt'
$ grep -c '^Remv' /tmp/qt-dryrun.txt   →  1
$ grep -c '^Inst' /tmp/qt-dryrun.txt   →  23
```

`Remv` 行只有 1 条容易误读成「只删一个」，但方括号里是**全部级联受害者**
（`plasma-workspace`、`kwin-x11`、`sddm` 等 200+）。且 `apt-cache search`
确认**不存在 gles 变体的 dev 包**，无绕开方案。

因此 ARM64 走「x86 moc + arm64 编译器 + sysroot 里的 Qt 头/库」这条路，
完全不动板端包管理器。

## 与现有代码的边界

- 不修改 `client/` 下任何现有文件
- 不修改根 `CMakeLists.txt`
- 不修改 `scripts/`
- 不新增 D-Bus 方法（全部使用现有只读方法）

## 下一步

> **2026-11-06 首次审计**列出 5 项：代码层面已无孤儿接口，每个 `QLabel` 都有唯一写者，
> 每个 `JsonReader` / `DataSource` 公开方法都有真实调用者。**但「代码写完」不等于「可交付」**。
>
> **2026-11-06 五续**：首次审计的第 2（声明与实现不符）、4（UI 测试为零）、
> 5（eglfs 默认值）项已完成，明细见文末「本轮修复记录」。
> 剩余 **2 项**：HDMI 实机验收、`auto_scale` 产品决策。

### 1. HDMI 实机验收（唯一功能性阻塞项，仍待完成）

接显示器后走 `-platform eglfs` 实机验证。VNC 只能证明图形栈、光栅化与事件循环
是通的，**不能**替代 HDMI 时序、EDID 协商、分辨率切换与热插拔。

### 2. `auto_scale` 纵轴策略未决（仍待完成，需产品决策）

当前会把小波动放大到撑满全高（实测 20↔55 的 4 个低谷即撑满 121..561），
诊断场景下可能让「轻微波动」看起来像「剧烈抖动」。是否改固定 0–100、
或提供两种模式切换，**需要产品决策而非技术决策**，待实机看过效果再定。

### 其他（非阻塞）

- 视需要补充维度详情、告警高亮等交互
- 15 分钟长稳不等于周级；断网重连、服务重启等异常路径未覆盖
- `MainWindow` 的接线与 `DataFetchThread` 的并发仍无自动化回归
  （`LineChartWidget` 已补 8 条像素测试，但窗口层只靠板端截图人眼对账）

是否升格为正式 OpenSpec change：**待 HDMI 实机验证后再评估**。
当前代码仍是探索性质，未接入根构建、未修改任何既有文件。

## 本轮修复记录（2026-11-06 五续）

上一轮审计列出 5 项，本轮清掉 3 项，全部有可复算证据。

### ① `line_chart_widget.hpp` 声明与实现不符 → 补 8 条像素测试

`paint(QPainter*, QRect)` 之所以与 `paintEvent()` 分离、之所以不读
`this->width()`，**唯一动机就是可被像素测试**。本轮补上那个消费者：
`tests/test_line_chart_widget.cpp`（8 个用例，341 行）。

覆盖：空序列不画线、非空序列画线、**legacy 在中间断开连线**（带同进程对照组）、
legacy 点仍留灰色标记、单点不除零、全等值不贴边、换数据后 `legacy_` 与
`scores_` 保持等长、退化矩形安全返回。

判据刻意排除坐标轴文字行列（上一轮抗锯齿边缘 `0x88` 混白落到 `0x9E` 导致假失败），
并用**变异验证**证明断言可失败：把 `if (legacy_.at(i))` 改成 `if (false)` 后
3 条用例立即失败，改回后 `git diff` 为空。

### ② UI 层自动化测试为零 → 数据层同时补到 39 条

新增 3 条数据层用例（`PrimaryIssueSurvivesWithoutSummaryTemplate` 等），
数据层从 36 条升到 **39 条**。配合上面的 8 条像素测试，
`ctest` 现在覆盖 JSON 解析 + 图表绘制两层，仅剩窗口接线无自动化。

### ③ `main.cpp` 的 eglfs 默认值与实现不一致 → 补 `apply_default_platform()`

头注释写「默认走 `eglfs`」但代码没设置任何默认值。现在由
`apply_default_platform()` 落实，优先级为：
环境变量已设 → 不动；显式 `-platform` → Qt 自行覆盖；`--screenshot` → `offscreen`；
其余 → `eglfs`。

三场景实测（x86 无 DRM）：
- 不设环境变量、不带 `--screenshot` → 报 `Could not initialize egl display`（eglfs 生效）
- `--screenshot` 且不设环境变量 → 成功保存 1024×700 PNG（自动切 offscreen）
- 已设 `QT_QPA_PLATFORM=offscreen` → 正常运行（不被覆盖）

注：`--screenshot` 必须抢在 `QApplication` 构造前判断，所以手工扫 `argv`，
不能用 `QCommandLineParser`（构造时平台插件已定）。

### ④ 顺带修掉的四个真问题（不在首次审计的 5 项内）

**初始化失败会永久卡死取数线程。** `weaknet_init()` 在 `client.cpp:1348` 的实现是
`g_client` 非空时直接 `return g_client->isConnected()`，而 `connected_` 只在
`initConnection()` 里赋值一次。连接失败后全局客户端仍留着，于是后续每次
`weaknet_init()` 都走「已初始化」分支、永远返回 `false` —— 板端开机早期
D-Bus 未就绪时，看板就再也连不上了。修法：`DataSource::init()` 失败路径调用
`weaknet_cleanup()` 把全局客户端置空（副作用是 `Logger::shutdown()`，
已实测 glog Init/Shutdown 反复循环安全），`DataFetchThread::run()` 改为按
2s 退避重试并切片睡眠以响应 `quit_requested_`。

**状态栏「就绪」会吃掉失败提示。** 原来 `onRefreshFinished` 靠检查状态栏文字
是否以「失败」开头来决定是否复位，但那是跨轮持久状态：第一轮失败、第二轮成功时
第二轮会被误判成仍失败。改成每轮独立的 `round_had_failure_` 标志。

**`DiagnosisSnapshot::primary_issue` 是孤儿字段。** 解析了从未读过。
按「给已解析字段接入真实消费者」处理：`summary_template` 为空时用它兜底显示
「首要问题 xxx」，两者都空则隐藏。补了 3 条用例覆盖（含空串不等于缺失）。

**标题与实际查询不符。** `-i` 不传时标题停在 `build_ui()` 的
「历史趋势（分数）」，但实际查的是全部网卡，且 `history_limit_` 也停在类默认值。
现在 `main.cpp` **无条件**调用 `setInterface()`（不传则传空串），
`build_ui()` 不再写标题，`setInterface()` 成为唯一标题写者。
帮助文案也从「默认自动检测」改成「不传则查询全部网卡」——「自动检测」在代码里不存在。
