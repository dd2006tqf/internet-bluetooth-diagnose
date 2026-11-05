# Qt 看板代码骨架与完整调用链

> 日期：2026-11-05
> 分类：探索 / 工作流外实施
> 结果：**ARM64 已构建并上板跑通**，真实 D-Bus 取数 5/5 成功，离屏渲染做像素级验证；
> 仅剩 HDMI 实机显示（显示器未在位）

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
qt_dashboard/                          19 个文件，代码 3192 行（含 2 个构建脚本）
├── README.md                  设计决定与验证状态
├── CMakeLists.txt             139 行，独立 project，三 target
├── main.cpp                   入口，QCommandLineParser
├── data/                      ← 不依赖 Qt
│   ├── json_reader.hpp/cpp    178 / 478 行，路径式查找 + 根数组支持
│   ├── dashboard_data.hpp/cpp 301 / 235 行，POD + 解析
│   └── data_source.hpp/cpp    152 / 197 行，调 libweaknet + 缓冲扩容
├── ui/                        ← 依赖 Qt
│   ├── line_chart_widget.hpp/cpp  106 / 208 行，QPainter 折线图
│   ├── data_fetch_thread.hpp/cpp  115 / 120 行，QThread 后台取数
│   └── main_window.hpp/cpp        116 / 336 行，主窗口
├── tools/
│   ├── build-arm64.sh                ARM64 交叉编译（x86 moc + arm64 clang）
│   └── fetch-qt-arm64-sysroot.sh     往 sysroot 装 arm64 Qt 头/库
└── tests/
    ├── test_json_reader.cpp   34 个用例（含 6 个真机样本）
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

### 后续

1. 接 HDMI 显示器，走 `-platform eglfs` 实机验证（**唯一剩余项**）
2. 视需要补充维度详情、告警高亮等交互

是否升格为正式 OpenSpec change：**待 HDMI 实机验证后再评估**。
当前代码仍是探索性质，未接入根构建、未修改任何既有文件。
