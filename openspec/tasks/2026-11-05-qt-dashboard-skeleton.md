# Qt 看板代码骨架与完整调用链

> 日期：2026-11-05
> 分类：探索 / 工作流外实施
> 结果：调用链已打通并**在本机完成完整编译、链接、启动验证**；ARM64 交叉编译与板端运行未做

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
qt_dashboard/                          16 个文件，代码 2988 行
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
└── tests/
    └── test_json_reader.cpp   385 行，30 个用例
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
| 数据层单测 | ✅ **30/30 通过** | `ctest` 0.03s |
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
| ARM64 交叉编译 | sysroot 无 Qt dev，需 D2（改 `scripts/` 初始化脚本）或 D3（Docker 容器） |
| 上板运行 | 无 HDMI 显示器（当前 `disconnected`） |
| 真实 D-Bus 取数验证 | 只验证了程序能启动，**未验证 `weaknet_get_*` 是否真的取到数据** |
| 界面渲染效果 | 无显示器，offscreen 只能证明不崩溃 |

### 后续

1. 解决 ARM64 交叉编译环境（D2 或 D3）
2. 接显示器后走 `-platform eglfs` 实机验证
3. 验证真实数据：确认 D-Bus 取数成功、维度列表与趋势图正确渲染

是否升格为正式 OpenSpec change：**待 ARM64 构建 + 实机验证通过后再评估**。
