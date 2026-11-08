# 看板多指标趋势与现场交互三能力

> 日期：2026-11-08
> 分类：工作流外实施
> 结果：已完成

## 构思

### 问题

`qt_dashboard` 的趋势图只画综合分，且顶栏只有一个「刷新」按钮。这带来两个后果：

1. **诊断时看不到物理量**。综合分是归一化的合成结果，现场看到「分数掉了」无法判断
   是 RTT 变差、丢包变多，还是信号变弱。原始测量值（RTT / 抖动 / RSSI / 丢包率）
   早就存在 `network_history` 表里，只是一直没画出来。
2. **触摸是无消费者的孤儿**。`hid-multitouch` 外部模块 2026-11-07 已经验证可用，
   但产品定位 v1.2 承认「现场交互终端」之后，白名单里的三项交互（趋势指标切换 /
   时间窗切换 / 截图导出）一直停留在文档，代码里没有任何可点击点。驱动能力齐备
   但没有业务消费者，等于不能进生产链路。

### 为什么不做进 OpenSpec change

功能代码此前已在工作流外写好一部分（`dashboard_data` 的 `HistoryPoint` 已扩字段），
按项目铁律「代码已实现时不套用 change 流程追认」——追认会导致 footprint 为 0、
`--path` 与 diff 不符、`planning changed during command` 死锁。因此本任务按
工作流外实施处理，记录于本目录（不具备验收效力）。

### 备选方案与取舍

| 决策点 | 备选 | 选择 | 理由 |
| --- | --- | --- | --- |
| 指标来源 | A1 原始测量值 / A2 13 个 SLE 维度 | **A1** | 13 个 SLE 维度在 `network_history` 表里**没有存储列**，做不出趋势；A1 的五个指标都是真实列 |
| 24h 降采样 | B1 全量取点 / B2 用 limit 降采样 | **B1** | 服务端是 `ORDER BY ts DESC LIMIT ?`：`limit=1440` 会让**最新的 1440 条（约 4 小时）占满名额**，剩下 20 小时一条都没有。标签写 24h、图里只有 4h 是数据保真度上的谎报。改为按 `窗口秒数 / 10s` 算足 |
| 坐标计算 | 留在 `LineChartWidget` / 抽出独立层 | **抽出 `chart_geometry`** | 算得对不对是纯算术，应能直接断言，不该只能靠数像素间接反推 |
| Y 轴策略 | 全部自适应 / 分数自适应 + 物理量固定基准 | **后者** | 有绝对物理含义的量（dBm、ms、%）自适应会把 `-45↔-50 dBm` 放大成满屏断崖 |
| 截图落盘 | 只写 U 盘 / 只写 `/tmp` / U 盘优先 + `/tmp` 兜底 | **兜底方案** | 没插 U 盘就导不出图，等于现场取不了证据；但兜底必须明示实际路径 |

## 实现方式

### 数据层（本机可验证）

`qt_dashboard/data/dashboard_data.hpp` / `.cpp`：

- `HistoryMetric` 枚举：`Score` / `Rtt` / `Jitter` / `Rssi` / `TcpLoss`
- 四个哨兵常量 `kNoRttMs = -1` / `kNoJitterMs = -1` / `kNoRssiDbm = -1000` /
  `kNoTcpLossPct = -1`，与 `database_manager.cpp` 的 `null` 语义一一对应
- `HistoryPoint` 扩字段 + `value(metric)` / `has(metric)`
- `is_history_metric_absent()`、`history_metric_label()`、`history_metric_unit()`、
  `history_metric_range()`、`history_window_*()` 四个时间窗纯函数
- `parse_history()` 的 fallback 从 `0.0` 改为**哨兵值**——`0.0` 会让「没测到 RTT」
  显示成「RTT = 0ms」，是最好看的假象

`qt_dashboard/data/data_source.{hpp,cpp}`：

- `fetch_history_raw()` 增加 `start` / `end` 参数
- `kMaxBuffer` 从 4 MB 上调到 12 MB：24h 需要 8640 点 × 约 784 字节 ≈ 6.46 MB，
  原上限会导致 24h 窗口永远取数失败（报错而非静默截断）
- 在头注写明「7 天需 60480 点，超过服务端 `kMaxHistoryLimit = 10000`，因此不提供 7d」

### 几何层（不依赖 Qt，本机可验证）

新建 `qt_dashboard/ui/chart_geometry.{hpp,cpp}`：`compute_axis_range()` /
`map_point()` / `axis_label_decimals()`。该 target 不 include 任何 Qt 头，
因此可在没有 Qt 开发包的机器上编译测试。

固定基准是**下限而不是硬裁剪**：数据在基准内就取基准（波动看起来是真实幅度），
超出就向外长（不裁掉最该被看见的极值）。

### UI 层

`qt_dashboard/ui/line_chart_widget.{hpp,cpp}`：

- 三个平行 `QVector`（scores / labels / legacy）合并为单个 `PlotPoint` 向量。
  既有测试 `LegacyVectorStaysInSyncWithScores` 专门守三向量等长——
  「需要写测试守的不变量，就说明设计上有可以误用的缝」
- `setMetric()` 只重算视图，不重新取数
- 指标名与单位由 `paint()` 自己拼进标题，避免「图上画的是 RSSI，标题写着综合分」
- `legacy` 断开连线**仅在 Score 指标下成立**：它标的是「评分模型是旧的」，
  不影响 RSSI 这类原始测量值的可比性
- 两种断点分开处理：`absent` 不画点、不留标记；`break_line` 不连线但画灰色空心圆

`qt_dashboard/ui/main_window.{hpp,cpp}`：白名单三项落地

| 白名单项 | 实现 |
| --- | --- |
| 趋势指标切换 | `QComboBox`（5 项，存 enum 值而非下标）+ `onMetricSelected()` |
| 时间窗切换 | 三个互斥可勾选 `QPushButton`（1/6/13h）+ `onWindowSelected()` |
| 截图导出 | `export_screenshot()` + `onExportClicked()` |

关键约束：**切指标不重置时间窗，切时间窗不重置指标**。两条都有专门测试。

`find_usb_dir()` 的判据踩过一个真坑，写在代码注释里：`QStorageInfo` 对**普通目录**
也会返回 `isReady() == true`，此时 `rootPath()` 是所属文件系统挂载点（通常是 `/`）。
只看 `isReady` 会把 `/media/<user>` 这种空壳当成 U 盘，把图写进 rootfs。
正确判据是 **`rootPath()` 必须等于候选路径本身**，并排除 proc / sysfs / tmpfs 等伪文件系统。

### 验证

本机虽然有 `qtbase5-dev-tools`（提供 moc）但没有 `qtbase5-dev` 本体，
不过 `/tmp/qtstage2` 下有一份 Qt5 开发头文件与库，因此 **UI 层也能真的编译验证**：

```bash
cmake -S qt_dashboard -B /tmp/bqd-ui \
  -DWEAKN_DASHBOARD_WITH_UI=ON \
  -DWEAKNET_CLIENT_LIB=$PWD/build-x86/client/lib/libweaknet.so \
  -DCMAKE_PREFIX_PATH=/tmp/qtstage2/root/usr/lib/x86_64-linux-gnu/cmake
cmake --build /tmp/bqd-ui -j$(nproc)

export LD_LIBRARY_PATH=/tmp/qtstage2/root/usr/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH
export QT_PLUGIN_PATH=/tmp/qtstage2/root/usr/lib/x86_64-linux-gnu/qt5/plugins
export QT_QPA_PLATFORM=offscreen
ctest --test-dir /tmp/bqd-ui --output-on-failure
```

结果：**5/5 测试套件全绿**（`dashboard_data` / `chart_geometry` /
`line_chart_widget` / `theme` / `main_window`），`-Wall -Wextra` 零警告。

新增测试：

- `test_json_reader.cpp`：+15 条（`HistoryMetricTest.*` 10 条 + `HistoryWindowTest.*` 5 条）
- `test_chart_geometry.cpp`：新建 12 条
- `test_line_chart_widget.cpp`：+4 条多指标像素测试
- `test_main_window.cpp`：新建 8 条交互测试

### 测试过程中抓到的两个真缺陷

1. **`is_legacy_pixel()` 误吞抗锯齿折线边缘**。绿色 `#2E7D32` 与白底按
   t≈0.4~0.47 混合得到约 `(140,180,140)`，三个通道都落在灰色容差
   `158±28` 内。修法：灰色标记要求**三通道彼此接近**（`max-min ≤ 12`）。
2. **`find_usb_dir()` 把 `/media/<user>` 当成 U 盘**（见上）。
   测试现象是「状态栏报了路径，但文件找不到」——图被写进了
   `/media/nomachine`（权限 777 的 rootfs 普通目录）。

两个都是产品代码缺陷，不是测试写错。

### 2026-11-08 续：板端真机验证（补完遗留项 1）

原记录把「未在板端真机验证」列为遗留项。本轮补上，并因此又抓到**三个**新缺陷。

#### 缺陷 A：`build-arm64.sh` 漏编 `chart_geometry.cpp`（链接失败）

新增的 `ui/chart_geometry.cpp` 只加进了 `qt_dashboard/CMakeLists.txt`，
没加进 `qt_dashboard/tools/build-arm64.sh` 的 `SOURCES` 数组。两套构建配置
各自维护文件清单，于是 x86 CMake 构建全绿、ARM64 交叉编译直接链接失败：

```text
undefined reference to `weaknet::dashboard::compute_axis_range(...)`
undefined reference to `weaknet::dashboard::axis_label_decimals(...)`
undefined reference to `weaknet::dashboard::map_point(...)`
```

修法：把 `ui/chart_geometry` 插到 `SOURCES` 里（放在使用它的
`ui/line_chart_widget` 之前，便于按依赖顺序阅读）。

> **结构性教训**：一个源文件同时出现在两套构建清单里，就必然有漂移风险。
> 本轮只做了最小修补，没有把 `build-arm64.sh` 改成从 CMake 读取源文件列表——
> 那属于构建体系重构，超出本任务范围。若后续再发生同类漏编，应该真正合并清单。

#### 缺陷 B：时间窗按钮的选中态在真机上完全不可见

`build_view_controls()` 里三个时间窗按钮是 `setCheckable(true)`，
逻辑上始终「恰好一个 `isChecked()`」，`WindowButtonsAreMutuallyExclusive`
测试也一直绿。但真机截图上三个按钮**渲染完全一致**：

| 按钮 | 边框像素 | 内部像素 |
| --- | --- | --- |
| 1 小时（选中） | `(224,227,232)` | `(255,255,255)` |
| 6 小时 | `(224,227,232)` | `(255,255,255)` |
| 24 小时 | `(224,227,232)` | `(255,255,255)` |

根因：QSS 只写了 `QPushButton:hover` 与 `:pressed`，**没有 `:checked`**。
而 QSS 一旦设置，就会整体接管该控件的绘制，把 Fusion 原生样式里本来存在的
选中高亮一并覆盖掉。修法是在 `theme.cpp` 补 `:checked` 与 `:checked:hover`
两条规则（深底 `%5`=`#212121` + 白字，不引入新语义色，避免绿色被误读成 GOOD）。

修复后板端复验：选中的「1 小时」内部主色变为 `rgb(33,33,33)`，
其余两个仍是 `rgb(255,255,255)`。

#### 缺陷 C（最值得记的一条）：新写的回归测试首版是「假通过」

补测试时第一版写成「拿 1 小时按钮和 24 小时按钮互相比像素」——
**这条断言恒真**：两个按钮文案不同，像素天然不同。缺陷存在时它照样 PASS。

改成「同一个按钮在选中态与未选中态之间比较」后，**仍然 PASS**。
第二个原因更隐蔽：`test_main_window.cpp` 从头到尾**没调用过 `apply_theme()`**，
量到的是 Fusion 原生样式，而缺陷恰好藏在全局 QSS 覆盖里。
不装主题 → 原生 checked 态自带高亮 → 测试 PASS → 缺陷被完美掩盖。

补上 `weaknet::dashboard::apply_theme()` 之后，RED 才真正成立：

```text
Expected: (b1_delta) > (100), actual: 0 vs 100
时间窗按钮在选中与未选中之间渲染完全相同（0 像素不同）：checked 状态对用户不可见
```

`0 像素不同`——这是一个量化到极致的红。

#### 验证命令与结果

```bash
# x86：本机 5 套件全绿
cmake --build /tmp/bqd-ui -j$(nproc)
export LD_LIBRARY_PATH=/tmp/qtstage2/root/usr/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH
export QT_PLUGIN_PATH=/tmp/qtstage2/root/usr/lib/x86_64-linux-gnu/qt5/plugins
export QT_QPA_PLATFORM=offscreen
ctest --test-dir /tmp/bqd-ui --output-on-failure
# → 100% tests passed, 0 failed out of 5

# ARM64：交叉编译 + 板端真机
./qt_dashboard/tools/build-arm64.sh          # 518008 B / glibc 2.17
scp build-qt-arm64/weaknet_dashboard board:/tmp/
ssh board 'QT_QPA_PLATFORM=offscreen ./weaknet_dashboard --screenshot /tmp/dash.png'
# → screenshot: 1024x757 saved
```

用例数（`--gtest_list_tests` 实测）：dashboard_data 55 / chart_geometry 12 /
line_chart_widget 12 / theme 9 / main_window 9，合计 **97**。

### 2026-11-08 续二：最大时间窗 24h → 13h（服务端能力上界反推）

#### 发现过程

在补完板端验证后核对「时间窗请求多少条」时，发现两处独立的事实错误：

**事实错误一：落库周期不是 10 秒，是 5 秒。**

初版 `kHistorySampleIntervalSeconds = 10`，依据是 CLAUDE.md 的
「数据采集间隔：10 秒」。但那是 `config.yaml` 里**各监控器**的
`interval_ms`（3~30 秒不等），与 `network_history` 表的写入节奏是两件事。
后者由 `server/src/server.cpp` 的 `start_history_persistence_thread()` 决定，
硬编码 5 秒。

板端实测（直调 `libweaknet` 取 20 分钟样本）：相邻记录间隔中位数 **5.0 秒**、
最大 6.0 秒，与代码一致。

后果：每个窗口的 limit 只有实际所需的一半。服务端是
`ORDER BY ts DESC LIMIT ?`——先倒序再截断，于是 24h 请求 8640 条只覆盖
约 12 小时，**标签写 24h、图里画 12h**。

**事实错误二：服务端会静默钳制 limit，而 24h 恰好越界。**

`server/src/dbus_service.cpp:1515` 的 `kMaxHistoryLimit = 10000` 是硬上界，
超出部分不会报错，直接截断。板端实测：请求 7 天 / `limit=20000`，
返回**恰好 10000 条**，时间跨度 69.99h 而非 7 天。

按真实 5 秒周期算：10000 × 5s = 50000s ≈ **13.89 小时**，这是能力上界。
24h 需要 17280 点，超出上限 73%。

**为什么之前测不出来**：板端历史目前有明显空档（24h 请求只取回 2610 条 /
2.19 MB），所以 24h 请求「碰巧」返回了完整的 24.00h 跨度。这是数据稀疏的
偶然，不是保证——设备连续运行、数据变密后，同一个 24h 按钮就会开始说谎。

#### 决策

把最大窗口从 24h 改成 **13h**（9360 点，留 640 点余量）。

> 这不是能力退化，而是把假能力换成真能力：**13h 全量可见 > 24h 只看到 14h**。
> 挂一个必然说谎的按钮，比少一个窗口更坏——现场会据此判断「过去一天都正常」，
> 而那 10 小时根本没被看到。

#### 改动范围

| 层 | 文件 | 改动 |
| --- | --- | --- |
| 数据层 | `data/dashboard_data.hpp` | `HistoryWindow::Last24h` → `Last13h`；`kHistorySampleIntervalSeconds` 10 → 5；补齐三个周期的区分表 |
| 数据层 | `data/dashboard_data.cpp` | 窗口秒数 24h → 13h；label 文案 |
| 取数层 | `data/data_source.hpp` | limit 表按 5 秒重算；新增「为什么最大是 13h」「字节数是上界不是常态」两节 |
| 取数层 | `data/data_source.cpp` | `kMaxBuffer` 注释按 10000 点满载（≈7.5 MB）重写 |
| UI 层 | `ui/main_window.hpp/.cpp` | 按钮成员与文案；修订两处仍按 10s/24h/60480 写的注释 |
| UI 层 | `ui/theme.cpp` | `:checked` 注释里的按钮文案 |
| 测试 | `tests/test_json_reader.cpp` | 三窗口时长/点数/上限断言；新增 `SamplingIntervalComesFromPersistenceThread` |
| 测试 | `tests/test_main_window.cpp` | 全部 24h 引用改 13h（含选中态可见性测试） |
| 文档 | `qt_dashboard/README.md` | 缓冲上限段落、构建清单、验证状态表（57 项计数订正为 97） |
| 文档 | `docs/产品定位-工业无线诊断网关.md` | 白名单 1h/6h/24h → 1h/6h/13h；60480 点/28 小时的重写为能力上界推导 |
| 文档 | `docs/架构设计-五层权责契约.md` | 同上 |

#### 复验证据

```bash
# x86：5 套件 97 用例全绿
cmake --build /tmp/bqd-ui -j$(nproc)
ctest --test-dir /tmp/bqd-ui --output-on-failure
# → 100% tests passed, 0 failed out of 5  (1.15s)

# ARM64：交叉编译
./qt_dashboard/tools/build-arm64.sh
# → 518008 字节 / 最高 glibc 需求 GLIBC_2.17

# 板端渲染
scp build-qt-arm64/weaknet_dashboard board:/tmp/
ssh board 'QT_QPA_PLATFORM=offscreen /tmp/weaknet_dashboard --screenshot /tmp/dash13.png'
# → screenshot: 1024x757 saved to /tmp/dash13.png  (109019 字节)
```

板端二进制的决定性证据（文案确实换了）：

```text
1 小时         utf16= 0  utf8= 2
6 小时         utf16= 0  utf8= 2
13 小时        utf16= 1  utf8= 2      ← 存在
24 小时        utf16= 0  utf8= 0      ← 彻底消失
13 小时（约 9360 个采样点，服务端上限内最优覆盖）  utf8= 1
```

渲染对账：板端与 x86 同主题截图里，顶栏控件块的包围盒**逐项一致**
（白块 47..170 / 325..395 / 406..484 / 511..598，深色选中块 243..315），
说明布局未因文案变短而退化，且选中态样式在两平台一致。

## 结果

**已完成**。四个批次全部落地，并已通过 ARM64 真机验证：

| 批次 | 内容 | 验证 |
| --- | --- | --- |
| 1 数据层扩充 | 5 指标 + 4 哨兵 + 时间窗纯函数 | 本机单测 |
| 2 图表支持指标切换 | `chart_geometry` 抽取 + `setSeries/setMetric` | 本机单测 + 像素测试 |
| 3 UI 接线 | 指标下拉框 + 三个时间窗按钮 | offscreen 交互测试 + 板端截图 |
| 4 截图导出 | U 盘优先 + `/tmp` 兜底 + 状态栏明示路径 | offscreen 端到端测试（回读 PNG 魔数） |
| 5 板端真机验证（新增） | 交叉编译修复 + 选中态修复 | ARM64 编译 + 板端 1024x757 截图逐像素对账 |
| 6 时间窗上界订正（新增） | 落库周期 10s→5s 勘误、最大窗口 24h→13h | 板端样本实测间隔 + 服务端钳制实测 + 二进制文案检索 |

文档同步：`docs/产品定位-工业无线诊断网关.md` 第三节之二、
`docs/架构设计-五层权责契约.md` 第四节、
`驱动面能力引入与业务融合方案.md` §7.6.2 / §10.2 / §14 均已从
「维度切换 / 代码待做」订正为实际实现状态。

### 遗留

1. ~~**未在板端真机验证**~~ → **2026-11-08 已补**（见上一节）。已覆盖：
   ARM64 交叉编译、板端启动、D-Bus 真实取数、五个指标下拉框与三个时间窗
   按钮真实渲染、选中态视觉可分辨。**仍未覆盖**：eglfs 直出（无显示器）、
   实际触摸点按（`hid-multitouch` 已有驱动能力，但没有触摸屏在位）、
   U 盘热插拔。
2. **无 U 盘时落 `/tmp` 掉电即失**。状态栏已明示，但若现场需要持久证据，
   应考虑写入 `/home/radxa/weaknet/data/`（该路径由 OTA 布局管理，需先确认持久性）。
3. ~~**7 天窗口受服务端 10000 点上限阻塞**~~ → **已量化并落到产品里**：
   10000 × 5s ≈ 13.89h 是硬上界，因此把最大窗口定在 13h 并同步订正文档。
   若将来要更长历史，需要服务端先支持降采样/聚合查询
   （当前 `queryHistory()` 只做过滤 + 截断，超限静默钳制）。
4. **两套构建清单并存有漂移风险**。`qt_dashboard/CMakeLists.txt` 与
   `qt_dashboard/tools/build-arm64.sh` 各自维护源文件列表，本轮已因漏编
   `chart_geometry.cpp` 出过一次链接失败。理想做法是让脚本从 CMake 读源
   文件列表，但那属于构建体系重构，未在本轮做。
5. **板端交互（点击 / 触摸）仍未在真机验证**。已验的是「渲染」：二进制里
   文案正确、控件块几何与 x86 一致、选中态配色可见。未验的是「真机上点一下
   会发生什么」——无触摸屏在位，脚本也未向真机注入鼠标事件。交互语义目前
   只由 x86 `offscreen` 的 `test_main_window.cpp`（9 例，走真实槽）覆盖。
   待触摸屏到货后补。

### 是否升格为正式 OpenSpec change

不建议。功能已完整落地并有测试覆盖，再走 change 流程属于「追认」，
会因 footprint 为 0 与 fingerprint 反复变化而报废（见 CLAUDE.md 硬性规则）。
若后续要在板端做真机验证并记录为受管证据，那应作为**新的** change
（例如「看板板端真机验证」），而不是给本次实现补追溯。
