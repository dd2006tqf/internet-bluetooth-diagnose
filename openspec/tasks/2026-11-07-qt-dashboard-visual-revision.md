# Qt 看板视觉改版

- **日期**：2026-11-07
- **类型**：工作流外任务（UI 视觉优化，不涉及 OpenSpec change）
- **触发**：用户要求「参考 GitHub 上的 Qt 页面优化界面」

## 构思

### 问题诊断（先量后改）

改之前先做了一次界面审计，结论不是「主观觉得丑」，而是三条可复算的事实：

1. **零主题层**：全仓没有 `.qss` 文件、没有 `qApp->setStyleSheet()`、
   没有 `QPalette` 设置、没有 `QStyleFactory` 调用。
2. **颜色散落**：状态色 `#2E7D32` / `#F9A825` / `#C62828` 在三个文件里
   各写一遍（`state_color()`、顶栏 summary 的 html、折线图 `line_color_`）。
3. **圆点是死的**：`make_status_row()` 创建了 `●` 标签但从没把它交给任何人，
   14 行圆点恒为默认前景色——界面上有圆点，却不携带任何状态信息。

### 参考依据

WebSearch 多次返回空，改为直接查 Qt 官方文档（这是实际的「参考」来源）：

- `stylesheet-examples.html`：`QGroupBox` 需要 `margin-top` 给标题让位；
  `::title` 需要 `subcontrol-origin: margin` 且背景与卡片同色才能「切开」边框；
  `QPushButton` 必须显式写 `border`，否则渲染成扁平无边框。
- `stylesheet-syntax.html`：同等特异性下后写的规则胜出，因此
  `:hover` / `:pressed` 必须写在基础规则之后。
- `qpainter.html`：闭合 `QPainterPath` + `QLinearGradient` + `setClipRect`
  实现曲线下面积填充。

### 关键技术不确定性（先用探针消掉）

**全局 QSS 与部件级 `setStyleSheet` 会不会打架？**

`update_status_label()` 用部件级 `setStyleSheet("color: ...")` 做运行时状态着色。
如果引入全局 QSS 会覆盖它，就得重写整套着色逻辑。写了两个一次性探针实测
（`/tmp/qsstest/t2.cpp`、`t4.cpp`）：

- 全局 QSS 设红色、部件级设蓝色 → 两个控件各自正确（267px / 308px）；
- 全局 QSS 设 `font-size`、部件级只设 `color` → 两边笔画高度都是 22px。

结论：**两者共存，部件级 color-only 不会重置全局字号**。
这条实测直接决定了方案可以保留现有的运行时着色，不必推倒重写。

## 实现方式

### 新增 `ui/theme.{hpp,cpp}`

- `ThemeColors`：10 个语义色常量，成为唯一色源。
- `build_stylesheet()`：约 140 行 QSS，7 个 `%n` 占位符由 `.arg()` 按序替换。
- `build_palette()`：QPalette 兜底（原生对话框、popup 等 QSS 覆盖不到的地方）。
- `apply_theme()`：创建 Fusion 样式 → 设 palette → 设全局 QSS，返回实际样式名。

在 `main.cpp` 的 `QApplication` 构造之后、`MainWindow` 构造之前调用。

### 顶栏三级分层

原来是一个扁平 `QHBoxLayout`，六项信息同一字重。改为：

- 左：刷新按钮（`PointingHandCursor`）+ 网卡名（等宽 12pt 加粗）
- 中：分数（`score_value`，右对齐 + 固定最小宽度防抖动）+ 状态汇总色块
- 右：档位与时间戳竖排（`meta_info`，9pt 弱化，右对齐）

### 圆点复活

`make_status_row()` 改为双出参（`QLabel** state_label, QLabel** dot_label`），
`MainWindow` 新增 13 个 `QLabel* xxx_dot_` 成员，
`update_status_label()` 签名扩为三参，内部用 lambda 给圆点与文字同色。

### 折线图

- 颜色全部改从 `ThemeColors` 取；删掉已成常量的成员 `line_color_`
  （`setLineColor()` 早前作为孤儿接口被删，成员只是把常量伪装成状态）。
- 新增曲线下 `QLinearGradient` 填充。关键细节：填充必须按**连续 v2 段**闭合
  （`runs` 记录每段起止），跨 legacy 段填充会画出一块凭空多出来的三角。

### 布局硬约束

`QGroupBox` 的 `margin-top` / `padding` 和大字号分数都会抬高窗口最小高度。
这条有明确门槛（720p 屏幕必须放得下），因此字号与间距是被测量结果反向约束的，
不是审美选的。

## 结果

### 测试

| 套件 | 结果 |
|------|------|
| `dashboard_data` | 39/39 通过 |
| `line_chart_widget` | 8/8 通过 |
| `theme`（新增） | 9/9 通过 |

`-Wall -Wextra` 零警告，干净目录重新 configure 后仍然零警告。

### 同源 A/B 对照（同一块板 radxa-cubie-a7a，同一份 D-Bus 数据）

用 HEAD 源码重新交叉编译出旧版 ARM64 二进制，在同一台板子上各截一张图：

| 指标 | 改进前 | 改进后 |
|------|--------|--------|
| 截图尺寸 | 1024x769 | 1024x717 |
| 背景主色 | `(239,239,239)` 79.8% | `(245,246,248)` 57.1% |
| 卡片白 `#FFFFFF` | 0 px | 122515 px |
| 分隔线 `#E0E3E8` | 0 px | 2796 px |
| GOOD 绿 | 1256 px | 2326 px |
| DEGRADED 琥珀 | 828 px（旧色） | 625 px（新色） |
| `minimumSizeHint().height()` | 679 px | 661 px |

渐变填充的像素证据：x=600 列上 `g−r` 从 y=310 的 +9 单调降到 y=530 的 +1，
而纯 `#2E7D32` 折线仍有 683px —— 填充没有污染线色。

### 被测试拦下的两个真实缺陷

1. **琥珀色对比度不足**：初版沿用 Material Amber 800 `#F9A825`，
   新写的 WCAG 对比度测试算出 **1.97:1**，连非文本要求的 3:1 都不到。
   换成 `#B26A00`（4.24:1）后通过。
2. **最小高度撑破 720p**：初版 26pt 分数 + 10px padding 把最小高度从 679
   顶到 **728 px**，`resize(1024,600)` 被弹回 728。收到 20pt + 6px 后降到 661 px。

这两个都不是「测试写错了」，是设计确实有问题，且**只有量化才看得见**。

### 变异验证

把 13 个 `update_status_label()` 调用的 dot 参数全改成 `nullptr`，
`DotsFollowTheirStateColor` 立即失败并输出
「圆点可能没跟随状态着色（漏传 dot 参数）」，改回后恢复通过。
证明这条断言不是恒真条件。

## 未做的事 / 边界

- **HDMI 实机验收仍未完成**：显示器未在位（`HDMI disconnected`）。
  本轮全部证据来自 `offscreen` 平台插件的 `grab()`，走完整绘制路径，
  但不覆盖 HDMI 时序、EDID 协商、热插拔与 `/dev/dri` 权限。
- **`auto_scale` 纵轴策略仍未决**：需产品决策，未动。
- **窗口层接线仍无自动化回归**：`test_theme.cpp` 覆盖了渲染与状态着色，
  但 `DataFetchThread` 的并发与信号时序还是只靠板端截图人眼对账。
- **深色主题未实现**：`build_palette()` 注释里写了扩展点，但没做。
