# 文档同步：V4l2FrameSource 落地后的状态订正 + 测试套件数 59 → 60

> 日期：2026-11-09
> 分类：快速修复（文档与代码同步）
> 结果：已完成（8 个文件，全量 ctest 60/60 复验通过）

## 构思

### 要解决什么问题

`7e4ef2e`（2026-11-09 09:50）落地了 `V4l2FrameSource`，但仓库文档仍停留在它不存在
的状态，形成两类事实漂移：

**一、能力状态漂移（会低估完成度、也会误判边界）**

| 文档 | 过时表述 | 实际情况 |
| --- | --- | --- |
| `产品定位` 第八节 | 「`V4l2FrameSource` 未落地」 | 代码已落地（`7e4ef2e`，10 项单测） |
| `产品定位` 第三节之二 | 「该闭环要等 `V4l2FrameSource` 落地后」 | 落地了，但**未接进评估循环** |
| `驱动面方案` §9.1 | 「挡在前面的是 overlay + `V4l2FrameSource` 实现」 | 只剩 overlay + 接进评估循环 |
| `驱动面方案` §9.2 任务 8 | 未记录代码已落地 | — |
| `驱动面方案` §15.1 | 表格缺 `V4l2FrameSource` 行 | — |
| `驱动面方案` §15.2 第 4 项 | 同上 | — |
| `驱动面方案` §14 速查 | 「合计 48 项单测」 | 实为 58 项（+V4l2 的 10 项） |

危险点不只是「少记了一件好事」：把「代码已存在但未接线」写成「代码不存在」，
会让下一个接手的人重新实现一遍 `V4l2FrameSource`，并误以为 10 项单测不存在。

**二、测试套件数漂移（会误导验证判断）**

`7e4ef2e` 让套件数从 59 变成 60，但六处文档仍写 59。套件数是读者判断
「测试是否跑全了」的唯一锚点，写小了会让人以为有套件没被跑到。

### 为什么不做成 OpenSpec change

纯文档事实订正，零行为变更、零生产表面变化。按
`docs/ai/工作流使用指南.md`「改 typo、改注释 → 不用工作流」，以及项目 memory
`no-new-openspec-change-by-default`（用户 2026-11-06 明确要求不主动开 change）。

### 备选方案与取舍

| 方案 | 取舍 |
| --- | --- |
| A. 只把「未落地」改成「已落地」 | ❌ 会制造新假象——让人以为视频归因已经在工作 |
| **B. 一律改成「代码已落地，但未接进评估循环」（采用）** | ✅ 既纠正事实，又保住「生产态恒 `NONE`」这条真实边界 |
| C. 顺带把帧源接进 `server.cpp` | ❌ 那是行为变更（会让归因可能产出非 `NONE`），且无真机可验，属另一件事 |

选 B。**本轮严格只同步事实，不碰任何生产代码路径。**

## 实现方式

### 改了哪些文件（8 个）

| 文件 | 改动 |
| --- | --- |
| `docs/产品定位-工业无线诊断网关.md` | ①第三节之二：视频闭环的阻塞项从「帧源落地」改为「真机出帧 + 接进评估循环」；②第八节视频行：补 `7e4ef2e`、10 项单测，明确「未接进 `server.cpp`、未真机出帧」 |
| `驱动面能力引入与业务融合方案.md` | ①§6.2.6 第 2 条依据：`未接入（无摄像头）` → `未接进评估循环（server.cpp 仍用默认构造的空 FrameStats）`；②§9.1 结论：只剩 overlay + 接线；③§15.1 新增 `V4l2FrameSource` 完成行；④§14 速查：48 → 58 项单测，并补「代码已落地」；⑤§15.2 第 4 项：改为「摄像头 overlay 启用 + 帧源接进评估循环」；⑥§15.2 第 1 项的 `帧源未接` → `帧源未接进评估循环` |
| `CLAUDE.md` | 快速操作指南：59 套件 → 60 套件（受管模板，已跑 `manifest_sync.sh`） |
| `.ai-harness/manifest.json` | 由 `manifest_sync.sh` 自动更新 `CLAUDE.md` 的 `content_sha256` |
| `README.md` | 全量测试注释：59 → 60，耗时改为实测 8 秒 |
| `docs/ci-测试使用指南.md` | 三处 59 → 60；**并补上缺失的 `test_v4l2_frame_source_gtest` 表格行**（此前该套件不在文档表中） |
| `docs/skills-and-openspec-guide.md` | 快速操作表：59 → 60 套件 |
| `docs/交叉编译与开发板部署.md` | 构建方式对比表：59 → 60 套 GTest |
| `docs/架构设计-模块与代码结构.md` | ①`server/src` 的 `.cpp` 计数 66 → 70；②监控矩阵新增 `weaknet_softirq` 行；③在 thermal 段后补「其余两条同形态归因」表（observer / video），并写明三条归因的共同形态与 `decide()` 之后的定位 |

### 关键决策一：为什么区分「未落地」与「未接进评估循环」

这两句话对外部读者的含义完全不同：

- **未落地**：这个能力还没写，谁想用谁去写；
- **未接进评估循环**：能力已在仓库里、单测已覆盖、接口已定，只差一行装配。

前者会诱发重复劳动，后者才是当前真实状态。**这也是本轮唯一新增的措辞约定**——
后续所有「代码完成 ≠ 能力完成」的场合都沿用这个区分。

### 关键决策二：`docs/架构设计-模块与代码结构.md` 的 `.cpp` 计数

顺带发现文档写 66、实际 70。核实方式不是数目录，而是确认每个文件都在构建里：

```bash
find server/src -name '*.cpp' | wc -l              # 70
find server/src -name '*.bpf.c' | wc -l            # 12
for f in $(find server/src -name '*.cpp' | sed 's|server/src/||'); do
  grep -q "$f" server/CMakeLists.txt || echo "NOT IN CMAKE: $f"
done                                                # 零输出 = 全部在构建里
```

零输出说明这 70 个文件都不是孤儿源码，计数订正是安全的。

### 验证手段

```bash
# 1. 套件数权威值（不是数文档，是问 ctest）
ctest --test-dir build-x86/server -N | tail -3
ctest --test-dir build-x86/server --output-on-failure | tail -4

# 2. 文档中的旧值是否清零
git grep -nE "59 套件|59 个套件|59 个 gtest|59 套 GTest" -- "*.md"

# 3. 受管模板哈希是否一致
python3 -c "...遍历 managed_paths，逐个 sha256 比对..."   # 见下
scripts/manifest_sync.sh

# 4. 文档表述与 V4L2 代码逐项对照
grep -n "1280\|720\|buffer_count" server/include/v4l2_frame_source.hpp
grep -n "v4l2:" server/src/v4l2_frame_source.cpp
```

实测结果：

| 检查 | 结果 |
| --- | --- |
| `ctest -N` 总数 | `Total Tests: 60` |
| 全量 ctest | `100% tests passed, 0 tests failed out of 60`，7.84 s |
| 旧值 59 残留（套件语境） | 0 处 |
| manifest 模板哈希 | 0 处不匹配（`All hashes up to date`） |
| `V4l2FrameSource` 默认配置 | `width{1280}` / `height{720}` / `buffer_count{4}`，与文档一致 |
| `describe()` 前缀 | `v4l2:` + device + `WxH`，与文档一致 |

## 结果

### 最终状态

仓库中「`V4l2FrameSource` 未落地」的表述已全部清零，替换为更精确的
「代码已落地（`7e4ef2e`，10 项单测），但未接进 `server.cpp` 评估循环、未真机出帧」。
测试套件数在 7 个位置统一为 60，并与 `ctest` 实测一致。

### 遗留问题

1. **视频归因仍未真正工作**。本轮只改文档，`server.cpp` 里仍是
   `weaknet::FrameStats frames;`（默认构造，`valid()==false`），归因恒为 `NONE`。
   这是**有意保留**的——接帧源是行为变更，且无真机可验。
2. **板子当前不可达**，阶段 2 overlay 演练无法推进：
   `ssh board`（`127.0.0.1:2222`）连接被拒，`radxa-cubie-a7a.local` 也无法解析。
   演练手册的 Step 0（串口线验证）需要人在板边，远程做不到。
3. **`qt_dashboard` 仍未进发布链**（`grep qt_dashboard tools/` 零命中），
   属发布策略决策，本轮未动。

### 后续是否值得升格为 OpenSpec change

**不需要**。本轮是纯事实订正。

真正值得走 change 的是下一步：**把 `V4l2FrameSource` 接进 `server.cpp` 评估循环**。
那会让 `video_qoe_attribution` 从恒 `NONE` 变成可能非 `NONE`，是明确的行为变更。
但按 thermal 的先例（`server.cpp` 里三条归因都只做「追加解释」、不参与
`OverallPolicy`），届时应先评估是否仍需完整工作流，还是沿用 thermal / observer
的工作流外 + `openspec/tasks/` 记录模式。**前置条件是真机出帧**，在此之前
写接入代码等于对着假设编程。

## 相关

- `openspec/tasks/2026-11-08-v4l2-frame-source-and-overlay-playbook.md`（上一轮落地记录）
- `openspec/tasks/2026-11-08-product-positioning-status-sync.md`（定位文档 v1.2.1 的同步记录）
- `openspec/tasks/2026-11-08-stage2-overlay-drill-playbook.md`（阶段 2 演练手册，当前阻塞项）
- `docs/ai/工作流使用指南.md`（工作流适用性判断）
