# Qt 看板代码骨架与完整调用链

> 日期：2026-11-05（2026-11-06 续：孤儿清理 + 真实消费者接入；2026-11-06 五续：补 UI 测试与三个真 bug）
> 分类：探索 / 工作流外实施
> 结果：**ARM64 已构建并上板跑通**，真实 D-Bus 取数 3/3 成功，离屏渲染做像素级验证；
> VNC 平台插件跑通完整图形栈并做 15 分钟长稳（零泄漏、零错误）；
> 已删除全部孤儿接口并给已解析字段接入真实消费者（数据层 39/39 通过）；
> **代码层面已无孤儿，可交付性只剩 2 项**（HDMI 实机验收、`auto_scale` 产品决策）

## 构思

### 要解决什么问题

开发板接 HDMI 显示器时，能在板端本地查看 WeakNet 的诊断状态，不依赖
远程 SSH 或云端。这是根目录 `内核模块技术栈引入讨论.md` 方向一（路径 A1）。

### 为什么不走 OpenSpec change

1. 这是**探索性代码骨架**，不是已批准的产品表面。按 CLAUDE.md 铁律，
   没有 approved requirement、真实消费者和可观测命令的接口属于 orphan，
   不该走正式变更流程。
2. 用户明确指示「不用新开工作流。直接开始实现基本的代码结构」，
   后续又明确「把第一第二部做了，然后第三步等显示器；不用新开工作流」。
3. 代码放在独立目录 `qt_dashboard/`，不接入根构建，不修改任何现有文件，
   对主项目零影响。

### 备选方案与取舍

| 方案 | 取舍 |
|------|------|
| 复用 `client/weaknet_client.h` C API | **选它**。29KB 封装已就绪，50+ 只读函数，返回 JSON |
| 自己实现 D-Bus 客户端 | 弃。`weaknet_cli` 之外再写一套是重复 |
| 引入 nlohmann/json | 弃。全仓不用第三方 JSON，`net_info.cpp:15` 记录了该决定 |
| 引入 Qt Charts | 弃。板端无 `libqt5charts5`；且无法脱离显示器测试 |
| QPainter 自绘 + 渲染到 QImage | **选它**。可像素级单测，无显示器也能验证 |
| 接入根 CMakeLists.txt | 弃。看板是可选界面，不该拖累主部署产物构建 |
| 同步 D-Bus 调用放 UI 线程 | 弃。服务端忙时会冻结界面，必须有后台线程 |

## 实现方式

### 新增文件（全部在独立目录，未改动任何既有文件）

```
qt_dashboard/                          19 个文件，代码 4423 行（含 2 个构建脚本）
├── README.md                  设计决定与验证状态
├── CMakeLists.txt             164 行，独立 project，四 target
├── main.cpp                   181 行，入口，QCommandLineParser
├── data/                      ← 不依赖 Qt
│   ├── json_reader.hpp/cpp    160 / 450 行，路径式查找 + 根数组支持
│   ├── dashboard_data.hpp/cpp 281 / 223 行，POD + 解析
│   └── data_source.hpp/cpp    134 / 163 行，调 libweaknet + 缓冲扩容
├── ui/                        ← 依赖 Qt
│   ├── line_chart_widget.hpp/cpp  98 / 217 行，QPainter 折线图
│   ├── data_fetch_thread.hpp/cpp  115 / 157 行，QThread 后台取数 + init 重试
│   └── main_window.hpp/cpp        160 / 539 行，主窗口
├── tools/
│   ├── build-arm64.sh                ARM64 交叉编译（x86 moc + arm64 clang）
│   └── fetch-qt-arm64-sysroot.sh     往 sysroot 装 arm64 Qt 头/库
└── tests/
    ├── test_json_reader.cpp         39 个用例（数据层，含 6 个真机样本）
    ├── test_line_chart_widget.cpp    8 个用例（UI 像素级，QImage 断言）
    └── fixtures/
        └── board_experience_real.json   板端实测快照（带来源标注）
```

### 调用链

```
main.cpp   QApplication + 命令行
  └── MainWindow                 顶栏 / 维度列表 / 趋势图 / 诊断栏
        ├── DataFetchThread      QThread，后台取数（禁止在 UI 线程调 D-Bus）
        │     └── DataSource     唯一直接调用 libweaknet 的地方
        │           └── weaknet_get_*()  ← client/weaknet_client.h
        ├── LineChartWidget      QPainter 自绘折线图
        └── parse_xxx()          JSON → POD（纯函数，可单测）
```

### 关键决策

**1. JSON 样本逐字取自服务端代码**

测试里的每一段 JSON 都来自真实序列化代码，不是臆造：

| 样本 | 来源 |
|------|------|
| 完整快照 | `server/include/assurance/legacy_adapter.hpp:162-227` |
| 无快照 | `server/src/dbus_service.cpp:916-917` |
| 快照过期 | `server/src/dbus_service.cpp:919-921` |
| 历史数组 | `server/src/database_manager.cpp:703-740` |
| 历史不可用 | `server/src/dbus_service.cpp:1530` |
| 诊断事实 | `server/include/assurance/diagnosis_engine.hpp:44-85` |

这样服务端改字段名时测试会红，而不是等接上显示器才发现界面空白。

**2. 核对真实 JSON 形状时纠正了四处臆断**

| 字段 | 猜测 | 实际出处 |
|------|------|---------|
| 历史时间戳 | `timestamp_ms`（数字） | `ts` 是**字符串**（`database_manager.cpp:704`） |
| 历史分数 | `display_score` | `score` / `overall_score`（`:720-721`） |
| 诊断置信度 | `double [0,1]` | **字符串枚举** `HIGH`/`MEDIUM`/`LOW`（`diagnosis_engine.hpp:49`） |
| `excluded_causes` | `vector<string>` | `vector<{cause,reason}>`（`:65-66`） |

**3. 枚举值也猜错了**

`server/include/assurance/health_state.hpp:15-38` 的真实取值：

```cpp
enum class HealthState { GOOD, DEGRADED, BAD, UNKNOWN };
// coverage: FULL_FOR_PROFILE / PARTIAL / NONE
```

原设计写的是 `HEALTHY` / `FAILED` 和 `FULL`，**全错**。已修正到
header、`.cpp` 和测试断言（含注释）。

**4. schema v2 的降级形状需要单独处理**

服务端在无快照 / 快照过期时返回**顶层** `state`/`coverage`/`reason`，
没有 `overall` 对象。`parse_experience()` 先检测 `overall.state` 是否存在，
不存在就按降级形状解析，并置 `unavailable_reason`。看板据此显示
「等待评估」而不是 0 分。

**5. 服务端按 `ts DESC` 返回，解析时翻转为正序**

`queryHistory()` 的 SQL 是 `ORDER BY ts DESC LIMIT ?`，最新在前。
`parse_history()` 在末尾 `std::reverse`，让图表从左到右是时间正序。

**6. 缓冲区自动扩容，绝不把截断当成功**

C API 不报告截断。取数层先给 8 KB，用 `JsonReader::valid()` 判断完整性，
不合法就翻倍重试，上限 4 MB。截断的 JSON 一律视为失败。

**7. `JsonReader` 增加根数组支持**

原设计只从 `{` 开始查找，但 `GetHistory` 返回顶层数组。补了
`rootIsArray()` / `getRootObjectArray()` / `rootArraySize()` 三个方法。

### 验证手段（无 Qt 开发包的环境）

板端 `apt install qtbase5-dev` 会级联移除 200+ KDE 包，已排除：

```text
$ ssh board 'apt-get -s install qtbase5-dev > /tmp/qt-dryrun.txt'
$ grep -c '^Remv' /tmp/qt-dryrun.txt   →  1
$ grep -c '^Inst' /tmp/qt-dryrun.txt   →  23

Remv libqt5gui5-gles [5.15.2+dfsg-4]
     [libkf5quickaddons5 libkdecorations2-5v5 breeze plasma-workspace
      kwin-x11 plasma-desktop sddm dolphin konqueror kate kmail okular
      ...]  ← 200+ 个 KDE 包
```

`Remv` 行只有 1 条容易误读成「只删一个」，但方括号里是**全部级联受害者**。
且 `apt-cache search` 确认**不存在 gles 变体的 dev 包**，无绕开方案。

**但宿主机（Ubuntu 24.04 noble）完全不同**：`apt-get -s install qtbase5-dev`
的 `Remv` 行数为 **0**，108 个 `Inst` 全是新增。板端危险是因为
`libqt5gui5` 与 `libqt5gui5-gles` 互斥且 KDE 依赖 gles 变体；宿主机没装 KDE，
无此冲突。**宿主机直装是安全的**，版本 `5.15.13` 还比 bullseye 的 `5.15.2` 新。

当时因为先撞上 Docker/apt 网络超时、又没做宿主机 dry-run,才绕路用了 deb staging。
记录在此避免重复绕路。**未解决的是 ARM64**：cross sysroot 里没有 Qt dev，
arm64 包只能从 bullseye 源装，那正是会拆 KDE 的一套。

宿主机同样不装系统包，改为**下载 deb 解压到临时目录**：

```bash
# 1. 从 mirrors.aliyun.com/debian 下载 amd64 的 Qt 头文件 + 运行库 + moc
#    解压到 /tmp/qtstage/root（不安装到系统）
dpkg-deb -x qtbase5-dev_*.deb /tmp/qtstage/root
dpkg-deb -x qtbase5-dev-tools_*.deb /tmp/qtstage/root   # moc 真身在这里
dpkg-deb -x libqt5gui5_*.deb /tmp/qtstage/root
# ... libmd4c / libdouble-conversion / libicu / libpcre2-16 等间接依赖

# 2. 手动跑 moc
moc ui/line_chart_widget.hpp -o moc_line_chart_widget.cpp
moc ui/data_fetch_thread.hpp -o moc_data_fetch_thread.cpp
moc ui/main_window.hpp       -o moc_main_window.cpp

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

## 结果

### 已完成

| 项 | 状态 | 证据 |
|----|------|------|
| 数据层编译 | ✅ 零警告 | `-Wall -Wextra` 通过 |
| 数据层单测 | ✅ **34/34 通过**（含 6 个真机样本用例） | `ctest` 0.03s |
| 取数层编译 | ✅ 通过 | 链接真实 `weaknet_client.h`，签名全对 |
| UI 层 **moc** | ✅ **3/3 生成成功** | `Q_OBJECT`/信号槽声明语法正确 |
| UI 层编译 | ✅ **零警告** | `main.cpp` + 3 个 ui 源文件 + 3 个 moc 产物，共 7 个单元全部通过 |
| **完整链接** | ✅ **成功** | 365712 字节可执行文件，无 undefined reference |
| **实际运行** | ✅ **启动正常** | `--help` 正确输出；`offscreen` 下持续运行 8 秒不崩溃 |
| CMake 配置 + 构建 + ctest | ✅ 全流程跑通 | `-DWEAKN_DASHBOARD_WITH_UI=OFF` 路径 |
| 服务端 JSON 形状核对 | ✅ 逐字段对照真实序列化代码 | 见上表行号 |

### 编译过程中抓到的 3 个真实 bug（全部已修）

| # | 错误 | 后果 |
|---|------|------|
| 1 | `main_window.cpp` 缺 `#include <QTimer>` | `invalid use of incomplete type 'class QTimer'` |
| 2 | `state_color(snap.overall_state)`：`std::string` → `QString` 无隐式转换 | 编译失败 |
| 3 | 同上，另一处 `state_color(item.state)` | 编译失败 |

这证明「未编译验证」不是形式主义——**代码确实有错**。

### 未完成（明确边界）

| 项 | 原因 |
|----|------|
| **HDMI 实机显示** | 唯一剩余项。显示器未在位（`HDMI disconnected`），eglfs 真机路径未跑 |
| 长时间运行稳定性 | 只跑了数分钟级，未做长稳 |

### 2026-11-05 续：ARM64 构建 + 板端真实取数与渲染（已完成）

上一轮停在「未验证 D-Bus 取数」，本轮把它全部打通。

#### 突破点：moc 用 x86，编译器用 arm64

板端装 Qt dev 会拆 KDE（200+ 包），此路已死。真正的解法是：

> **moc 是构建期工具，产物只是展开 `Q_OBJECT` 宏的普通 C++ 源码，与目标架构无关。**

因此用宿主机 x86 的 `moc 5.15.13`（`qtbase5-dev-tools`，零卸载）+ sysroot 里的
arm64 Qt 头文件/运行库 + `clang --target=aarch64-linux-gnu`，**不需要 Docker、
不需要 QEMU、不需要碰板端包管理器**。固化为 `qt_dashboard/tools/build-arm64.sh`。

产物 `build-qt-arm64/weaknet_dashboard`：399 KB，最高 glibc 需求 **2.17**
（板端 2.31），依赖 `libQt5Widgets/Gui/Core/DBus` + `libweaknet.so`，板端四个
Qt 运行库实测齐全。

#### 真实取数：5/5 全部成功

写一次性探针直接调 C API（用完即删，未入库）：

| API | 结果 | 大小 |
|-----|------|------|
| `weaknet_get_network_experience` | ✅ | 4129 B，schema v2，`DEGRADED`/`PARTIAL`/55 |
| `weaknet_get_diagnosis` | ✅ | 285 B |
| `weaknet_get_history` | ✅ | 50317 B / 60 条 |
| `weaknet_get_interfaces` | ✅ | 5 B（**裸字符串 `wlan0`，不是 JSON**） |
| `weaknet_health_check` | ✅ | 534 B |

`interfaces` 返回裸字符串是个**易踩的坑**：它不是 JSON。当前看板没有消费它，
所以未受影响；将来接入时必须按纯文本处理，不能丢给 `JsonReader`。

> **后续变化（2026-11-06 四续）**：`DataSource` 未加 `interfaces` / `health`
> 的取数方法后，`weaknet_get_interfaces` 与 `weaknet_health_check` 在看板侧
> 无任何消费者，属于孤儿，已连同 `FetchResult` 的对应字段一并删除。
> 因此「真实取数」从 5/5 收窄为 **3/3**（experience / diagnosis / history）。
> 这两个 C API 本身仍在 `client/weaknet_client.h` 里，由 `weaknet_cli` 使用，
> 不是项目级孤儿。

#### 真实渲染：像素级差分验证

给 `main.cpp` 加 `--screenshot <path> [--screenshot-delay ms]`，用 `offscreen`
平台插件跑真实 arm64 二进制，首轮取数完成后 `window.grab()` 存 PNG。

| 检查 | 真实数据（wlan0） | 对照（`--interface nosuchif0`） |
|------|------------------|------------------------------|
| `y=341` 折线连续段 | **674 px**（329..1003，与 `plot` 边界吻合） | **无折线** |
| 图表中心提示文字 | **0 px** | **396 px**（"暂无历史数据"） |
| 状态色 绿/黄/红/灰 | 75 / 96 / 14 / 428 采样点 | — |

真实 60 个点 score 全为 55.0 → 折线是水平线且居中，与 `auto_scale` 的
`(hi-lo)<1e-6 → ±5` 兜底一致。

#### 顺手修掉的两个真问题

1. **`--interface` 是孤儿参数**：`main.cpp` 解析了却从未传给 `MainWindow`，
   属于「有接口无消费者」。补上 `MainWindow::setInterface()` 后才真正生效——
   上面那张对照组截图正是它生效的证明。
2. **`HistorySeries::score_range()` 是多余的第二套轴逻辑**：只被测试引用，
   而 `LineChartWidget::paint()` 内部另算了一套带边距的轴（含 ±5 兜底）。
   两个函数对同一概念给出不同答案，后来者必用错。经用户确认后**删除**，
   连带删掉 2 个只为它存在的测试。

#### 删除后回归：截图逐像素对比

改动前后各截一张 1024x700：

- 差异 **1042 px**，全部落在两处含运行时刻的文字区域
  （右上角时间戳 `y=20..32 x=966..1013`、底栏诊断 `y=611..622 x=451..1000`）
- 图表区、左侧维度列表、分数、状态色**完全一致**

证明重构没有改变渲染行为。

#### 测试与回归

| 项 | 状态 |
|----|------|
| 数据层单测 | ✅ **34/34 通过** |
| 其中新增真机样本用例 | ✅ **6 个**（`BoardRealSampleTest`） |
| 样本留存 | `qt_dashboard/tests/fixtures/board_experience_real.json`（带来源标注） |
| ARM64 重编译 | ✅ 零警告 |
| 板端重部署 + 重截图 | ✅ 通过 |

新增的 `BoardRealSampleTest` 与原有测试**不是重复**：原有样本抄自服务端
序列化代码（验证「代码怎么写」），新样本取自运行中的服务端（验证「实际返回什么」）。
本轮就靠它抓到两处真实差异：

1. `assessment_profile` 真机是 `INTERNET_ACCESS`，代码抄本写的是 `FULL`
2. 真机同时出现 GOOD/BAD/DEGRADED/UNKNOWN 四种状态，且有 `UNKNOWN + PARTIAL`
   这种「有覆盖但不可用」的组合——这是抄本样本覆盖不到的

**写测试时踩的坑**：我给历史样本写的是 ASC 顺序，而服务端实际按 `DESC` 返回
（`parse_history` 负责翻转）。测试失败暴露的是样本错了，不是代码错了。
真实样本必须保留原始顺序，否则测不到翻转逻辑。

### 2026-11-05 再续：折线「贴顶 + 沉底」排查（结论：渲染正确，无 bug）

上一轮收尾时发现截图里折线大部分贴在绘图区顶端（y=121）、还有一处垂直
下探到 y=561，而当时查数据库看到最近记录全是 55.0，怀疑是渲染或解析错误。
本轮把它查清，**结论是折线渲染完全正确**，错的是核对方法。

#### 关键教训：拿「此刻」的数据去核对「十几分钟前」的截图

第一轮核对查的是当前时刻最近 120 条，全是 55.0，于是断定折线形状不合理。
但截图是 14:08 拍的，而数据库查询已经跑到 14:20 之后，时间窗口早已移过去了。

按截图时间回查 `13:50–14:10` 窗口，真实分布是 **55.0 × 64 条 + 20.0 × 4 条**。
那 4 个 20 分低谷就是折线里的下沉点。

#### 几何反解证明渲染无偏差

先定位绘图区：网格线在 y=77/209/341/473/604（间距 132，5 条线），
即 `plot.top≈77.5`、`plot.bottom≈604.5`、`plot.height≈527`。

`LineChartWidget::paint()` 的 `auto_scale` 逻辑（`line_chart_widget.cpp:108-124`）
对 min=20 / max=55 会算出 span=35、pad=3.5 → lo=16.5、hi=58.5。

| 数据点 | 计算 y_ratio | 计算 y | 实测 y |
|--------|-------------|--------|--------|
| 55.0（主体，最大值） | (55−16.5)/42 = 0.9167 | 604.5 − 0.9167×527 = **121.4** | **121** ✓ |
| 20.0（低谷，最小值） | (20−16.5)/42 = 0.0833 | 604.5 − 0.0833×527 = **560.6** | **561** ✓ |

两项都精确吻合。折线画的是序列的最大值和最小值——这正是 `auto_scale`
上下各留 10% 边距的定义，**不是 bug**。

#### 预测性验证：换一个数据窗口再截一张

查此刻最近 120 条，分布是 `55.0 × 120`（全等值）。按代码逻辑预测：
全等值时 `span < 1e-6` 走 ±5 兜底（lo=50, hi=60），55 分应落在
ratio=0.5，即 **y=341 的水平线**。

板端用 `QT_QPA_PLATFORM=offscreen ./weaknet_dashboard --screenshot` 重截：

| 检查项 | 预测 | 实测 |
|--------|------|------|
| 绿色折线所在行 | y=341 | **y=341，673 px** ✓ |
| 图表中心「暂无历史数据」(#999999) | 0 px | **0 px**（严格匹配）✓ |

预测与实测一致，反证了折线渲染与解析链路正确。

#### 排查中差点踩的坑：像素检测的容差

第一次做「中心提示文字」检测时用了 ±30 容差匹配 `#999999`，结果报
「640 px → 有提示文字」，与视觉和逻辑都矛盾。原因是容差把折线的抗锯齿
过渡色 `#8EB690` / `#8AB28C` 也算进去了。改成**严格相等**后结果是 0 px。

**教训**：像素断言的容差必须小于相邻语义色之间的距离，否则会把
「有数据」误报成「无数据」。同类错误在上一轮也出现过一次——
扫描 `y=341` 固定行找折线，数据一变就失效，后来改成全行扫描。

#### 未修改任何代码

本次排查没有改动仓库内任何源文件：结论是渲染正确，不需要修。
一次性探针脚本（`inspect_png.py` / `trace_line.py` / `ascii_png.py` /
`probe_history.c`）全部在 `/tmp`，未入库。

### 2026-11-05 三续：VNC 后端 + 15 分钟长稳（补齐「持续运行」项）

HDMI 仍为 `disconnected`（`/sys/class/drm/card0-HDMI-A-1/status`），
但板端 Qt 插件目录里有 `libqvnc.so`，于是改用 **VNC 平台插件**验证：

- 它走完整的 QPA 图形栈（光栅化、输入事件分发、GUI 事件循环），
  只是输出到网络而非 HDMI，比 `offscreen` 更接近实机
- 不需要显示器、不需要改板端包管理

#### 真实帧缓冲取景验证（不是截图 API）

用 Python 手写 RFB 3.3 客户端连 `127.0.0.1:5902`，完成握手后请求
FramebufferUpdate，拿到**真实像素**：

```text
banner:            RFB 003.003
security type:     1 (None)
framebuffer:       1024x700 bpp=32 depth=32 truecolor=1
desktop name:      Qt for Embedded Linux VNC Server
rects:             1  (0,0 1024x700)
pixels:            716800
```

关键区域检测结果：

| 检查项 | 结果 |
|--------|------|
| 图表区绿色折线行 | `y=344`，686 px（水平居中；比 offscreen 的 341 差 3px 是窗口边距差异）|
| 图表中心 `#999999` 提示文字 | **严格匹配 0 px** —— 确有数据 |

这条路子比 `window.grab()` 更强：`grab()` 是应用自己截图，VNC 是**外部客户端
读取真实帧缓冲**，证明画面确实被光栅化并推送出去了。

#### 15 分钟长稳

| 时刻 | ELAPSED | RSS | fd | 线程 | CPU | 日志错误 |
|------|---------|-----|----|----|-----|---------|
| 首测 | 04:12 | 52116 kB | 9 | 3 | 0.9% | 0 |
| 中测 | 09:56 | 60300 kB | 9 | 3 | 0.8% | 0 |
| 终测 | 15:19 | 60300 kB | 9 | 3 | 0.8% | 0 |

RSS 从 52 MB 抬到 60 MB 后**完全持平**：连续 10 次、每 30 秒的采样值
均为 `60300 kB`，逐次不变。说明前期抬升是 VNC/渲染缓冲的一次性分配，
**无泄漏**。fd 与线程数恒定，日志自始至终只有 7 行（无任何 error/fail/segfault）。

期间每 10 秒一轮 D-Bus 取数（约 90 轮）持续跑，未出现连接失效或崩溃。

#### 边界（仍然成立）

- VNC 验证的是**图形栈与渲染**，不是 HDMI 时序、EDID、分辨率协商、
  热插拔这些只在物理链路上暴露的问题
- 15 分钟不等于长稳周级；断网重连、服务重启等异常路径未覆盖
- 因此 **HDMI 实机仍是唯一剩余验收项**，不能拿 VNC 顶替

### 后续（2026-11-05 当时判断，已被文末「下一步」取代）

1. 接 HDMI 显示器，走 `-platform eglfs` 实机验证（**唯一剩余项**；
   VNC 已验证图形栈，但 HDMI 时序/EDID/热插拔仍需实机）
2. 视需要补充维度详情、告警高亮等交互
3. **趋势图当前用 `auto_scale` 自适应纵轴**：数据只在小范围波动时会被
   放大到撑满全高（如 20↔55 的 4 个低谷就撑满 121..561）。诊断看板上
   这可能让「轻微波动」看起来像「剧烈抖动」。是否改为固定 0–100 纵轴、
   或提供两种模式切换，待实机看过效果后再定

---

## 2026-11-06 四续：孤儿清理（A）与真实消费者（B）

上一轮遗留两类问题：**A 类是没有任何消费者的接口**，**B 类是解析了却不给用户看的字段**。
两类都违反 CLAUDE.md 的 orphan 铁律——「没有 approved requirement、真实消费者和
可观测命令的接口属于 orphan：移除，或交回 Planner」。

### A 类：删除无消费者的接口

| 删除对象 | 依据 |
|---------|------|
| `LineChartWidget::clear()` / `pointCount()` / `setLineColor()` / `setAutoScale()` + `auto_scale_` 成员 | 全仓零调用；`auto_scale_` 恒为 true，条件判断已是死代码 |
| `JsonReader::rootIsArray()` / `rootArraySize()` / `raw()` | 全仓零调用；`getRootObjectArray()` 已覆盖根数组场景 |
| `EvidenceItem` 结构体 | 只有声明，既无解析也无消费者 |
| `FetchResult::buffer_used` 字段 | 写入 4 次、读取 0 次 |
| `DataSource::initialized()` | 只有 `data_source.cpp` 内部用私有成员，公开 getter 零调用 |
| `DiagnosisSnapshot::raw_json` | 只是 JSON 副本，无任何读取方 |
| `HistoryPoint::overall_quality` / `iface`、`HistorySeries::interface_name` / `empty()` | `overall_quality` 与 `score` 是同一信息的两种编码；`interface_name` 取首点 iface，多网卡查询时是错的 |

对应测试同步删除断言（`ReadsRootArray` 等改为直接验证 `getRootObjectArray()`）。

### B 类：给已解析字段接入真实消费者

| 字段 | 消费者 | 语义 |
|------|--------|------|
| `ExperienceSnapshot::count_by_state` | 顶栏 `summary_label_`（html 彩色 ●） | 一眼看出有几个 BAD/DEGRADED，不必逐行扫 |
| `assessment_profile` / `overall_coverage` | 顶栏 `profile_label_` | 不同档位阈值不同，不标出来会让「同样是 GOOD」看起来可比 |
| `warnings` | 底栏 `experience_detail_label_`（红色，无警告时隐藏） | 服务端主动标记的异常，不该被界面吞掉 |
| `primary_issue` | 并入 `experience_label_` 结论行 | 与诊断侧的故障域/置信度互为补充 |
| `summary_template` | 底栏 `summary_template_label_` | 服务端给的是完整人读句子 |
| `secondary_issues` / `excluded_causes` / `evidence_refs` | 底栏 `diagnosis_extra_label_` + tooltip | 计数上屏，明细进 tooltip（证据 ID 是机器标识，铺在界面上是噪声） |
| `actions[].priority` | 并入 `diagnosis_label_` | 只提示数量与最高优先级；看板不执行动作 |
| `SleItem::reason` | 每个状态标签的 tooltip | 解释「为什么是这个状态」，不占版面 |
| `HistoryPoint::legacy` | `LineChartWidget` 渲染 | v1 旧评分模型与 v2 不同量纲，**断开连线**并画灰色空心点 |
| `ExperienceSnapshot::schema_version` | `parse_experience()` 守卫 | 非 0 且非 2 时直接标成不可用，避免按 v2 路径静默取空 |

### 控件归属：每个 QLabel 只有一个写者

`experience` 与 `diagnosis` 是两个独立的异步数据源，到达顺序不保证。
共用一个 QLabel 会让后到者覆盖先到者的内容，因此底栏按来源拆分控件：

```
experience_label_        ← experience（等待评估 / 状态 + 首要问题）
experience_detail_label_ ← experience（警告，无警告时隐藏）
diagnosis_label_         ← diagnosis （故障域 + 置信度 + 动作数）
summary_template_label_  ← diagnosis （人读摘要）
diagnosis_extra_label_   ← diagnosis （次要问题 / 排除原因 / 证据）
```

任一侧刷新失败或晚到都不会抹掉另一侧已经显示的内容。

### 验证证据

| 项 | 命令 | 结果 |
|----|------|------|
| 数据层单测 | `cmake --build` + `ctest --test-dir /tmp/qt-final` | **36/36 通过**（原 34，新增 schema 守卫正/负两项） |
| ARM64 交叉编译 | `BUILD_DIR=/tmp/qt-b-arm64 ./qt_dashboard/tools/build-arm64.sh` | 通过，**10/10 个编译单元零警告**，产物 413744 字节，最高 glibc 需求 `2.17` |
| 板端离屏截图 | `QT_QPA_PLATFORM=offscreen ./weaknet_dashboard --screenshot` | 1024x742（比改动前 700 高 42px，正是新增两行控件） |
| 新控件真的渲染 | 纯 Python 解码 PNG 后按 x/y 分段统计像素 | 顶栏色块 `绿●8 琥珀●1 红●0 灰●4`，与 `busctl GetNetworkExperience` 返回的 13 维度状态**逐项一致**；档位 `INTERNET_ACCESS / PARTIAL` 与真机一致 |
| 隐藏控件不占版面 | 同上，扫描文字行 | 底栏只有 3 行可见文本；警告行与 extra 行在无数据/无警告时确实不占位 |
| `legacy` 断开连线 | 一次性探针（同进程对比，唯一差别是中间点的 `legacy`） | 非 legacy：`green_mid=1, grey_plot=0`；legacy：`green_mid=0, grey_plot=8`。**PROBE PASS** |

> 探针教训：第一版判据「中间列无绿色」FAIL 过一次，原因是统计了 Y 轴刻度文字的
> 抗锯齿边缘（`0x88` 混白后恰好落在 `0x9E`）。改为只统计绘图区内部，
> 并在同一进程内跑对照组，判据才真正能区分两条渲染路径。
> 单点像素断言必须配对照组，否则恒真条件也会「通过」。

### 两个刻意的设计取舍

1. **没有新增 `MainWindow::renderToImage()`**：`main.cpp` 的 `--screenshot`
   走 `window.grab()` 已经触发 `paintEvent → paint()`，板端 VNC 外部取帧
   也验证过同一条链路。再加一个没人调用的方法只会造出新的孤儿。
2. **`history`: 用请求侧网卡做图表标题，不取数据里的 `iface`**：查询全部网卡时
   序列里混着多个 iface，取任意一条都会把标题写错。`MainWindow::interface_`
   记录请求侧事实，`setInterface()` 同步更新标题。


## 下一步（2026-11-06 审计 → 五续后更新）

> **首次审计（2026-11-06）**列出 5 项：代码层面已无孤儿接口，每个 `QLabel` 有唯一写者，
> 每个 `JsonReader` / `DataSource` 公开方法有真实调用者。
> **但「代码写完」不等于「可交付」**。
>
> **五续（2026-11-06）**：第 2（声明与实现不符）、4（UI 测试为零）、5（eglfs 默认值）
> 项已完成，明细见下方「2026-11-06 五续」。**剩余 2 项**。

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

---

## 2026-11-06 五续：补 UI 测试 + 三个真 bug

首次审计列出 5 项缺口。本轮补掉其中 3 项（第 2、4、5），
并在补测试的过程中发现并修掉 4 个此前未记录的**真 bug**。

### ① 补 `tests/test_line_chart_widget.cpp`（8 个用例，341 行）

首次审计第 2 项指出：`paint(QPainter*, QRect)` 与 `paintEvent()` 分离、
不读 `this->width()`，**唯一动机就是可被像素测试**，但仓库里没有那个测试，
分离设计本身就成了只有测试才会用的孤儿接口。本轮补上消费者。

| 用例 | 断言要点 |
|------|---------|
| `EmptySeriesShowsNoLine` | 空序列绘图区内折线像素严格为 0 |
| `NonEmptySeriesDrawsLine` | 对照组：非空序列必须有折线像素 |
| `LegacyBreaksTheLineInTheMiddle` | 同进程对照：中间点 legacy → 中间列折线像素 0；全 v2 → >0；且两侧线段仍在 |
| `LegacyPointsAreStillMarked` | 全 legacy 无折线，但必须有灰色空心标记（不能静默消失） |
| `SinglePointRendersWithoutDivisionByZero` | n==1 时画点不除零 |
| `FlatSeriesDoesNotCollapseToEdge` | 全等值序列画在垂直中部（±6px），不贴顶/沉底 |
| `LegacyVectorStaysInSyncWithScores` | 连换两次数据后 `legacy_` 与 `scores_` 仍等长 |
| `DegenerateAreaIsIgnored` | 空矩形 / 小于边距的矩形安全返回 |

两条硬性判据纪律（上一轮踩坑后固化）：

1. **只统计绘图区内部**，排除坐标轴文字行列——第一版「中间列无绿色」假失败的
   原因是 Y 轴刻度文字抗锯齿边缘 `0x88` 混白后恰好落在 `0x9E`；
2. **每条断言配对照组**，同一进程内只改一个变量跑两次。单点断言没有对照组时，
   恒真条件也会「通过」。

**变异验证**（证明断言能失败）：把 `line_chart_widget.cpp` 里
`if (legacy_.at(i))` 改成 `if (false)` → 3 条用例立即失败；改回后 `git diff` 为空。

技术要点：`QWidget` 构造要求进程里先有 `QApplication`（`QGuiApplication` 不够），
用 gtest 全局环境 `::testing::AddGlobalTestEnvironment` 在首个测试前建、最后销毁；
CMake 里给该测试设 `ENVIRONMENT "QT_QPA_PLATFORM=offscreen"`，
否则无 X11 的机器上 Qt 直接 abort，测试会变成「环境挂了」而非「代码错了」。

### ② 数据层补 3 条用例（36 → 39）

配合下面第 ④ 项的 `primary_issue` 兜底逻辑，新增
`PrimaryIssueSurvivesWithoutSummaryTemplate`、`MissingPrimaryIssueAndSummaryBothEmpty`、
`EmptyStringPrimaryIssueIsNotFallback`。最后一条覆盖「空串不等于缺失」——
若只判 `empty()` 就返回，会把空串当成有效值写进界面。

### ③ `main.cpp` 补 `apply_default_platform()`

首次审计第 5 项：头注释写「默认走 `eglfs`」，代码却没设置任何默认值。
现在优先级明确为：

1. `QT_QPA_PLATFORM` 已设置 → 不动
2. 显式 `-platform <x>` → Qt 自行覆盖（会压过 `qputenv`）
3. `--screenshot` → `offscreen`（该选项用途就是「无显示器验证」，
   默认 eglfs 会在 `QApplication` 构造时因拿不到 DRM 直接失败，走不到截图逻辑）
4. 其余 → `eglfs`

`--screenshot` 必须抢在 `QApplication` 构造前判断，因此手工扫 `argv`，
不能用 `QCommandLineParser`（构造时平台插件已定）。

三场景实测（x86 无 DRM）：不设变量 → 报 `Could not initialize egl display`
（eglfs 生效）；`--screenshot` → 成功保存 1024×700 PNG；
已设 `QT_QPA_PLATFORM=offscreen` → 正常运行（不被覆盖）。

### ④ 顺带修掉的四个真 bug（不在首次审计的 5 项内）

**bug 1：初始化失败会永久卡死取数线程。**

`weaknet_init()` 在 `client/client.cpp:1348` 的实现是：

```cpp
if (g_client) return g_client->isConnected();
```

而 `connected_` 只在 `initConnection()` 里赋值一次。连接失败后 `g_client`
仍留在全局，于是后续每次 `weaknet_init()` 都走「已初始化」分支、
**永远返回 false**——板端开机早期 D-Bus 尚未就绪时，看板再也连不上，
而原 `DataFetchThread::run()` 在 init 失败时直接 `return`，线程直接退出。

修法两层：

- `DataSource::init()` 失败路径调用 `weaknet_cleanup()` 把全局客户端置空
  （副作用是 `Logger::shutdown()`，下次 init 再 `InitGoogleLogging()`——
  已实测 glog 的 Init/Shutdown 反复循环 3 轮安全）；
- `DataFetchThread::run()` 改为按 2s 退避重试，睡眠切成 100ms 片以响应
  `quit_requested_`；首次失败时发 `fetchFailed` + `refreshFinished`
  （后者是必需的，主线程靠它清 `refresh_in_flight_`，否则手动刷新按钮永久失效）。

**bug 2：状态栏「就绪」会吃掉失败提示。**

原 `onRefreshFinished` 靠检查状态栏文字是否以「失败」开头来决定是否复位。
但那是**跨轮持久状态**：第一轮失败、第二轮成功时，第二轮会被误判成仍失败，
永远停在错误文案。改成每轮独立的 `round_had_failure_` 标志，
`onFetchFailed` 置位、`onRefreshFinished` 读取并清零。

**bug 3：`DiagnosisSnapshot::primary_issue` 是孤儿字段。**

`dashboard_data.cpp:160` 解析了，`apply_diagnosis()` 里从未读过
（`:450`/`:452` 那两处 `snap.primary_issue` 属于 `apply_experience` 的
`ExperienceSnapshot`，不是同一个字段）。按「B 类：给已解析字段接入真实消费者」
处理：`summary_template` 为空时用它兜底显示「首要问题 xxx」，两者都空则隐藏。

**bug 4：`-i` 不传时标题与实际查询不符。**

`main.cpp` 原来只在 `parser.isSet(iface_option)` 时调 `setInterface()`，
不传 `-i` 时标题停在 `build_ui()` 的「历史趋势（分数）」，但实际查的是全部网卡，
且 `history_limit_` 也停在类默认值（100）而不是 `kHistoryLimit`(120)。
现在 **无条件**调用（不传则传空串），`build_ui()` 不再写标题，
`setInterface()` 成为唯一标题写者。帮助文案也从「默认自动检测」改成
「不传则查询全部网卡」——「自动检测」在代码里根本不存在。

### 验证证据

| 项 | 命令 | 结果 |
|----|------|------|
| 数据层单测 | `ctest --test-dir /tmp/qt-verify` | **39/39 通过**（实测输出 `39 tests from 5 test suites ran`） |
| UI 像素测试 | 同上 | **8/8 通过**（实测输出 `8 tests from 1 test suite ran`），变异验证确认可失败 |
| x86 完整链接 | `cmake --build` | 386856 字节，无 undefined reference（staging 重解压后复算；旧记录 404040 B 来自已清理的临时 staging，无法重现，以本次为准） |
| 三平台场景 | 手工三场景 | 不设变量 → `Could not initialize egl display`（eglfs 生效）；`--screenshot` → 存 1024x700 PNG（自动 offscreen）；已设 `QT_QPA_PLATFORM=offscreen` → 正常运行（不被覆盖） |
| 初始失败重试 | `DBUS_SYSTEM_BUS_ADDRESS=unix:path=/tmp/no_such_bus` | 每 2s 重试一次、每次 `weaknet_cleanup`，线程不退出；9 秒后仍能截图成功（66776 B，界面在、无数据） |
| ARM64 交叉编译 | `BUILD_DIR=/tmp/qt-arm64-refresh ./qt_dashboard/tools/build-arm64.sh` | 通过，**零警告**，产物 414472 字节，最高 glibc 需求 `2.17` |

### 五续后仍未完成

| 项 | 原因 |
|----|------|
| **HDMI 实机显示** | 显示器未在位（`HDMI disconnected`），eglfs 真机路径未跑 |
| `auto_scale` 纵轴策略 | 需产品决策（固定 0–100 / 双模式），待实机看效果 |
| 窗口层自动化 | `MainWindow` 接线与 `DataFetchThread` 并发仍只靠板端截图人眼对账 |

### 一条必须声明的证据边界

初始化重试的实测只覆盖了**持续失败不退出线程**这一半：
`DBUS_SYSTEM_BUS_ADDRESS` 指向不存在的 socket，观测到每 2s 重试一次、
每次调用 `weaknet_cleanup()`、线程存活、9 秒后界面仍能截图。

**没有**验证「环境恢复后自己连上」。要验证那一步需要在进程运行中途
把 D-Bus 环境修好，x86 上无法构造这个时序（改环境变量不影响已建连接，
建假总线又落回「一直成功」而非「先失败后成功」）。
该路径目前只有代码推理支撑：`weaknet_cleanup()` 把 `g_client` 置空并
`disconnect` → 下次 `weaknet_init()` 走完整重建 → `initConnection()` 重新赋值
`connected_`。**不得据此宣称「断线自动恢复」已实现。**

真机上的可行验证方式：板端开机时先让 D-Bus 未就绪（或临时 stop
`weaknet-server`）启动看板，再拉起服务，观察是否自动取到数。这属于
HDMI 实机验收同批可做的事。

---

## 会话收尾状态（2026-11-06）

本轮（「五续」）到此为止，代码与文档均已提交为 `db65efa`
（`feat(qt): 补 UI 像素测试并修掉四个真 bug，可交付性缺口从 5 项收到 2 项`），
外加一次文档措辞订正。

### 代码层面：可以结束

| 判据 | 状态 |
|------|------|
| 有无孤儿接口 | 无。A 类已删净，B 类字段全部接入真实消费者或被删除 |
| 每个 `QLabel` 是否只有一个写者 | 是（experience 2 个 / diagnosis 3 个，按数据源拆分） |
| 自动化测试 | 数据层 39 + UI 像素 8，共 47 条，`ctest` 全过 |
| 断言是否可失败 | 是。变异验证：破坏 legacy 分支 → 3 条立即失败 |
| 编译器警告 | 0（x86 GCC 13 / ARM64 Clang，`-Wall -Wextra`） |
| 能否交叉编译上板 | 能。414472 B / glibc 2.17 / 板端已跑通真实取数与渲染 |

### 不能结束的部分（2 项，都不是代码问题）

1. **HDMI 实机验收** —— 显示器不在位，无法做。这不是「代码没写完」，
   是「硬件没接」。VNC 已证明图形栈与渲染正确，但 HDMI 时序 / EDID /
   热插拔只能实机验证。
2. **`auto_scale` 纵轴策略** —— 需要产品决策（固定 0–100 还是双模式切换），
   按现有证据无法自行拍板。建议接上显示器后看一眼真实波动幅度再定。

### 下一会话若继续，从这里接

优先级建议：

1. 接 HDMI，跑 `sudo QT_QPA_PLATFORM=eglfs`，确认显示正常 —— 这是唯一功能性阻塞
2. 顺手做「初始化失败后自愈」的真机验证（先 stop `weaknet-server` 启动看板，
   再拉起服务，看是否自动取到数）。x86 上做不了，板端可以
3. 看过真实波形后再决定 `auto_scale`
4. 若要继续投入，再补 `MainWindow` 接线与 `DataFetchThread` 并发的回归测试

是否升格为正式 OpenSpec change：仍建议**待 HDMI 验收后再评估**。
当前代码不改任何既有文件、不接入根构建，对主项目零影响。
