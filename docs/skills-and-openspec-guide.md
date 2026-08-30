# Skills 与 OpenSpec 工作流使用指南

本项目由"tanqf"开发。

## 概述

本文档详细说明如何使用本项目集成的 **Superpowers Skills** 和 **OpenSpec 工作流**，包括具体命令和使用场景。

---

## 一、分层模型

```
┌─────────────────────────────────────────────────┐
│  工作流 (OpenSpec)                              │
│  定义：做什么流程，谁来做                        │
│  Planner → Generator → Evaluator 生命周期       │
├─────────────────────────────────────────────────┤
│  技能 (superpowers)                            │
│  定义：怎么把具体事情做好                        │
│  技术、模式、检查清单、反模式                    │
├─────────────────────────────────────────────────┤
│  项目规则 (AGENTS.md, CLAUDE.md)                 │
│  定义：项目特定约束和命令                        │
│  证据包装器、OpenSpec CLI、工具路径              │
└─────────────────────────────────────────────────┘
```

- **工作流** = 流程引擎（OpenSpec change 生命周期、角色职责）
- **技能** = 技术指南，Agent 在工作流中执行具体任务时调用
- **项目规则** = 本项目特定的约束和命令

**工作流规则和项目约束始终优先于技能指导。技能补充工作流，永不替代。**

---

## 二、OpenSpec 工作流

### 2.1 工作流概述

OpenSpec 是本项目唯一的变更管理流程，用于规划、实现和验证代码变更。

**核心原则**：
- 先规划、后实现、再记录证据
- 严格的步骤顺序，不可跳步
- 所有变更必须通过 change 管理

### 2.2 操作顺序（必须严格遵守）

```
# 规划阶段
1.  change_new.sh <name> --switch
2.  写 proposal.md / design.md / specs/ / tasks.md
3.  openspec_cli.sh validate <name> --strict
4.  evaluator_check.sh --plan
5.  snapshot_update.sh --freeze-planning-baseline

# 实现阶段
6.  snapshot_update.sh --freeze-implementation-base   # 先冻结！代码还没改
7.  修改代码                                          # 现在改代码
8.  change_footprint.sh <name> --json
9.  task_verify.sh <id> --phase regression ...        # 记录证据
    - within_expected → 不加 --drift-reason
    - drift_warning 等 → 必须加 --drift-reason
10. task_verify.sh --complete <id>

# 评估阶段
11. integration_surface_check.sh <name> --refresh
12. sync_hashes.sh <name>
13. evaluator_check.sh --begin
14. evaluator_check.sh --run ...
15. evaluation_template.sh <name>
16. evaluation_fix.sh <name>
17. pre_finish.sh <name>
18. evaluator_check.sh --finish

# 归档
19. change_archive.sh <name>
```

### 2.3 核心命令详解

#### **创建新 Change**
```bash
scripts/change_new.sh <name> --switch
```
- **作用**：创建新的变更，并切换到该 change
- **参数**：`<name>` 是 kebab-case 格式的变更名称（如 `add-rssi-alert`）
- **示例**：
  ```bash
  scripts/change_new.sh add-rssi-alert --switch
  ```

#### **验证 Change**
```bash
scripts/openspec_cli.sh validate <name> --strict
```
- **作用**：验证 change 的 proposal、design、specs 是否符合规范
- **参数**：`--strict` 启用严格模式

#### **冻结规划基线**
```bash
scripts/snapshot_update.sh --freeze-planning-baseline
```
- **作用**：冻结当前规划状态，作为后续实现的基线
- **时机**：在 validation 通过后、开始实现前

#### **冻结实现基线**
```bash
scripts/snapshot_update.sh --freeze-implementation-base
```
- **作用**：冻结当前代码状态，作为实现前的基线
- **时机**：在开始修改代码前（必须先冻结！）

#### **查看 Footprint**
```bash
scripts/change_footprint.sh <name> --json
```
- **作用**：查看变更的代码影响范围
- **输出**：JSON 格式的 footprint 信息
- **状态**：
  - `within_expected`：在预期范围内
  - `drift_warning`：有偏差，需要添加 `--drift-reason`
  - `review_required`：需要审查
  - `hard_exceeded`：超出范围，需要添加 `--drift-reason`

#### **记录证据**
```bash
scripts/task_verify.sh <id> --phase regression ...
```
- **作用**：记录任务完成的证据
- **参数**：
  - `<id>`：任务 ID
  - `--phase regression`：回归测试阶段
  - `--drift-reason`：当 footprint 状态不是 `within_expected` 时必须添加

**示例**：
```bash
# 在预期范围内
scripts/task_verify.sh 1 --phase regression --command "build-x86/server/test/test_quality_assessor_gtest"

# 有偏差时
scripts/task_verify.sh 2 --phase regression --command "..." --drift-reason "新增接口需要额外测试"
```

#### **完成任务**
```bash
scripts/task_verify.sh --complete <id>
```
- **作用**：标记任务为已完成

#### **集成检查**
```bash
scripts/integration_surface_check.sh <name> --refresh
```
- **作用**：刷新集成表面报告
- **时机**：所有任务完成后

#### **同步哈希**
```bash
scripts/sync_hashes.sh <name>
```
- **作用**：同步所有文件的哈希值

#### **评估开始**
```bash
scripts/evaluator_check.sh --begin
```
- **作用**：开始评估阶段

#### **运行评估**
```bash
scripts/evaluator_check.sh --run ...
```
- **作用**：运行评估命令

#### **生成评估模板**
```bash
scripts/evaluation_template.sh <name>
```
- **作用**：生成评估报告模板

#### **修复评估**
```bash
scripts/evaluation_fix.sh <name>
```
- **作用**：修复评估中发现的问题

#### **预完成检查**
```bash
scripts/pre_finish.sh <name>
```
- **作用**：完成前的最终检查

#### **评估完成**
```bash
scripts/evaluator_check.sh --finish
```
- **作用**：结束评估阶段

#### **归档 Change**
```bash
scripts/change_archive.sh <name>
```
- **作用**：归档完成的变更
- **时机**：所有验证通过后

### 2.4 铁律（必须遵守）

1. **遇阻塞必问**：遇到阻塞或错误必须停下来，向用户描述状态并等待决策
2. **禁止中途修改 harness 脚本**：修改 `scripts/` 目录会导致证据失效
3. **禁止中途修改 design.md**：一旦开始记录 evidence，绝对不能修改 design.md
4. **禁止手工编辑 evidence 文件**：只能通过 harness 命令维护
5. **创建 change 前必须清理工作区**：确保没有脏文件
6. **严格按照操作顺序执行**：不可跳步，不可调换顺序

### 2.5 快速操作指南

| 用户意图 | 执行命令 | 说明 |
|---------|---------|------|
| "开始新 change" | `scripts/change_new.sh <name> --switch` | 创建新 change |
| "归档" / "archive" | `scripts/change_archive.sh <name>` | 归档 change |
| "归档后部署" | `./tools/ci.sh --commit` | commit → push → 编译 → 部署 → 测试 |

---

## 三、Superpowers Skills

### 3.1 Skills 概述

Skills 是来自 [superpowers](https://github.com/obra/superpowers) 框架的技术指南，帮助 Agent 更好地执行具体任务。

**位置**：`skills/` 目录

**使用方式**：
- 告诉 Claude 使用某个 skill
- Claude 会根据任务自动识别并调用相关 skill

### 3.2 可用 Skills 列表

#### **设计与规划**

| Skill | 用途 | 使用时机 |
|-------|------|----------|
| `brainstorming` | 探索需求、设计方案 | 开始新功能前 |
| `writing-plans` | 编写详细实现计划 | 设计批准后 |

**brainstorming**：
```
使用 brainstorming 技能来探索这个新功能的设计
```
- **作用**：分类请求（spike/bounded/architectural），探索方案，呈现设计
- **输出**：设计方案，输入到 OpenSpec Planner 的 proposal/design 阶段

**writing-plans**：
```
使用 writing-plans 技能来编写实现计划
```
- **作用**：编写详细实现计划，包含 TDD 小步骤
- **输出**：详细计划，与 OpenSpec tasks.md 对齐

---

#### **执行**

| Skill | 用途 | 使用时机 |
|-------|------|----------|
| `executing-plans` | 执行已写好的计划 | 有详细计划时 |
| `subagent-driven-development` | 子代理驱动开发 | 大型任务并行开发 |
| `using-git-worktrees` | 隔离工作区 | 需要并行开发多个功能 |
| `finishing-a-development-branch` | 完成开发分支 | 任务完成时 |

**executing-plans**：
```
使用 executing-plans 技能来执行这个计划
```
- **作用**：逐任务执行已写好的计划
- **流程**：加载计划 → 评审 → 执行任务 → 完成

**using-git-worktrees**：
```
使用 using-git-worktrees 技能来创建隔离工作区
```
- **作用**：确保隔离工作区
- **注意**：创建 worktree 前必须获得用户明确同意

**finishing-a-development-branch**：
```
使用 finishing-a-development-branch 技能来完成开发
```
- **作用**：验证测试，提供集成选项，清理
- **OpenSpec 变更**：使用 `scripts/change_archive.sh`

---

#### **测试**

| Skill | 用途 | 使用时机 |
|-------|------|----------|
| `test-driven-development` | RED-GREEN-REFACTOR 循环 | 实现任何功能前 |

**test-driven-development**：
```
使用 test-driven-development 技能来实现这个功能
```
- **作用**：RED-GREEN-REFACTOR 循环
- **流程**：
  1. RED：写一个失败的测试
  2. GREEN：写最少的代码让测试通过
  3. REFACTOR：重构代码
- **注意**：所有行为变更必须遵循，除非有明确批准的例外

---

#### **调试**

| Skill | 用途 | 使用时机 |
|-------|------|----------|
| `systematic-debugging` | 4阶段根因调查 | 遇到 bug 时 |
| `verification-before-completion` | 完成前验证 | 声称完成前 |

**systematic-debugging**：
```
使用 systematic-debugging 技能来调试这个问题
```
- **作用**：4阶段根因调查
- **阶段**：
  1. 问题定义
  2. 根因分析
  3. 解决方案
  4. 验证

**verification-before-completion**：
```
使用 verification-before-completion 技能来验证完成
```
- **作用**：证据先于声明
- **要求**：在声明完成/通过前必须运行验证命令

---

#### **协作**

| Skill | 用途 | 使用时机 |
|-------|------|----------|
| `requesting-code-review` | 请求代码审查 | 任务完成时 |
| `receiving-code-review` | 接收审查反馈 | 收到审查意见时 |

**requesting-code-review**：
```
使用 requesting-code-review 技能来审查代码
```
- **作用**：完成任务后或合并前调度评审子代理

**receiving-code-review**：
```
使用 receiving-code-review 技能来处理审查反馈
```
- **作用**：技术评估评审反馈，而非表演性赞同

---

#### **元技能**

| Skill | 用途 | 使用时机 |
|-------|------|----------|
| `using-superpowers` | 如何发现和调用技能 | 开始任何对话时 |
| `writing-skills` | 创建/修改技能 | 需要新技能时 |

**using-superpowers**：
```
使用 using-superpowers 技能来了解可用技能
```
- **作用**：如何发现和调用技能

**writing-skills**：
```
使用 writing-skills 技能来创建新技能
```
- **作用**：遵循 TDD 原则创建或修改技能

---

### 3.3 Skills 详细说明

#### **brainstorming**

**位置**：`skills/brainstorming/SKILL.md`

**用途**：将想法转化为完整的设计和规范

**三种路径**：
- **Spike**：可行性问题，输出是答案而非代码
- **Bounded**：对现有代码的有限范围变更
- **Architectural**：新子系统或结构性变更

**流程**：
1. 检查当前项目状态
2. 评估范围
3. 逐个提问
4. 提出 2-3 种方案及权衡
5. 呈现设计，逐节询问
6. 将设计写入 `docs/superpowers/specs/` 或 OpenSpec design.md
7. 自我审查
8. 用户审查规范
9. 过渡到 writing-plans 技能

---

#### **test-driven-development**

**位置**：`skills/test-driven-development/SKILL.md`

**用途**：RED-GREEN-REFACTOR 循环

**RED-GREEN-REFACTOR 循环**：
1. **RED**：写一个失败的测试
2. **GREEN**：写最少的代码让测试通过
3. **REFACTOR**：重构代码

**反模式**（参考 `testing-anti-patterns.md`）：
- 测试实现细节而非行为
- 测试过于复杂
- 测试之间有依赖

---

#### **systematic-debugging**

**位置**：`skills/systematic-debugging/SKILL.md`

**用途**：4阶段根因调查

**4个阶段**：
1. **问题定义**：明确问题是什么
2. **根因分析**：找出根本原因
3. **解决方案**：制定解决方案
4. **验证**：验证解决方案

**参考文档**：
- `root-cause-tracing.md`：根因追踪技术
- `defense-in-depth.md`：深度防御技术
- `condition-based-waiting.md`：基于条件的等待技术

---

#### **verification-before-completion**

**位置**：`skills/verification-before-completion/SKILL.md`

**用途**：证据先于声明

**要求**：
- 在声明完成/通过前必须运行验证命令
- 提供具体的验证命令和预期结果

---

### 3.4 Skills 与 OpenSpec 配合使用

#### **场景 1：实现新功能**

```
1. 开始新任务
   └─→ 使用 brainstorming 探索设计方案
   └─→ 结果输入 OpenSpec Planner 的 proposal/design

2. 设计批准后
   └─→ 使用 writing-plans 创建详细计划
   └─→ 与 OpenSpec tasks.md 对齐

3. 实现过程中
   └─→ test-driven-development（写测试）
   └─→ systematic-debugging（调试）
   └─→ verification-before-completion（验证）

4. 完成时
   └─→ finishing-a-development-branch
   └─→ OpenSpec change_archive.sh 归档
```

#### **场景 2：修复 Bug**

```
1. 调查问题
   └─→ 使用 systematic-debugging 找出根因

2. 制定方案
   └─→ 使用 brainstorming 探索修复方案

3. 实现修复
   └─→ 使用 test-driven-development 写测试
   └─→ 修复代码

4. 验证
   └─→ 使用 verification-before-completion 验证修复

5. 完成
   └─→ OpenSpec change_archive.sh 归档
```

#### **场景 3：代码审查**

```
1. 完成任务
   └─→ 使用 requesting-code-review 请求审查

2. 收到反馈
   └─→ 使用 receiving-code-review 处理反馈

3. 修改代码
   └─→ 根据反馈修改

4. 再次验证
   └─→ 使用 verification-before-completion 验证
```

---

## 四、编译与测试命令

### 4.1 编译

| 用户意图 | 执行命令 | 说明 |
|---------|---------|------|
| "编译一下" / "build" | `cmake -B build-x86 -DCMAKE_BUILD_TYPE=Debug -DBUILD_EBPF=OFF && cmake --build build-x86 -j$(nproc)` | x86 本地快速编译 |
| "容器内编译" / "ARM64 编译" | `docker exec weaknet-arm64-dev bash -c 'cd /src && cmake -B build-x86 -DCMAKE_BUILD_TYPE=Debug && cmake --build build-x86 -j1'` | 需要 ~10 分钟（QEMU 模拟） |
| "编译 eBPF" | `docker exec weaknet-arm64-dev bash -c 'cd /src && cmake --build build-x86 --target ebpf -j1'` | 单独编译 eBPF 程序 |

### 4.2 测试

| 用户意图 | 执行命令 | 说明 |
|---------|---------|------|
| "跑测试" / "test" | `build-x86/server/test/test_database_manager_gtest` | 运行指定测试 |
| "跑全部测试" | `cmake --build build-x86 -j$(nproc) && ctest --test-dir build-x86/server -R "test_net_info\|test_quality\|test_anomaly\|test_audio\|test_band\|test_serializer\|test_event\|test_bt_full\|test_bt_monitor$\|test_iface\|test_logger\|test_traffic\|test_database"` | x86 单元测试 |

### 4.3 部署

| 用户意图 | 执行命令 | 说明 |
|---------|---------|------|
| "部署到板子" / "deploy" | `./tools/ci.sh` | 一键：编译 + 打包 + 部署 + 测试 |
| "归档后部署" | `./tools/ci.sh --commit` | git commit + push + 编译 + 部署 + 测试 |
| "只编译不部署" | `./tools/ci.sh --local-only` | ARM64 编译 + 打包，不 rsync |
| "部署但不测试" | `./tools/ci.sh --skip-test` | 跳过开发板测试 |

### 4.4 环境检查

| 用户意图 | 执行命令 | 说明 |
|---------|---------|------|
| "板子在线吗" / "检查开发板" | `ping -c 1 -W 2 192.168.2.77 && ssh radxa@192.168.2.77 'uname -m'` | 检查连通性 + 架构 |
| "容器状态" | `docker ps --filter name=weaknet-arm64-dev` | 检查 ARM64 容器 |
| "CI 状态" | `gh run list --limit 5` | 查看 GitHub Actions 运行记录 |

---

## 五、决策流程图

```
需要修改代码吗？
    │
    ├─ 是 → 使用工作流 (OpenSpec)
    │       └─ 严格遵守 CLAUDE.md 铁律
    │       └─ 在工作流中调用 Skills 执行具体任务
    │
    └─ 否 → 使用 Skill 或直接命令
            ├─ 审查代码 → /full-code-review
            ├─ 数据可视化 → /dataviz
            ├─ 配置 → /update-config
            └─ 其他 → 直接命令
```

---

## 六、注意事项

### 6.1 工作流优先级
- **修改代码** → 必须用工作流
- **不修改代码** → 可以用 Skill 或直接命令

### 6.2 Skill 限制
- Skill **不能**修改 OpenSpec 工件（proposal、design、tasks）
- Skill **不能**替代工作流的 evidence 记录
- Skill **不能**绕过 CLAUDE.md 的铁律

### 6.3 工作流限制
- 工作流**不能**用于简单的代码审查
- 工作流**不能**用于运行测试/部署
- 工作流**不能**用于配置 Claude Code

### 6.4 eBPF 编译注意事项
- **eBPF 必须在 ARM64 容器内编译**，x86 上会报 `user_pt_regs` 错误
- **容器内编译必须用 `-j1`**，QEMU 模拟下高并行度会 segfault
- **增量编译**：必须保留 build 目录做增量编译，切勿每次 `rm -rf build`

---

## 七、常见问题

### Q1：什么时候用 Skill，什么时候用工作流？

**A**：
- **修改代码** → 用工作流（OpenSpec）
- **不修改代码** → 用 Skill 或直接命令
- **在工作流中** → 用 Skill 执行具体任务

### Q2：Skills 可以替代工作流吗？

**A**：不可以。Skills 是技术指南，工作流是流程引擎。Skills 补充工作流，永不替代。

### Q3：如何知道该用哪个 Skill？

**A**：Claude 会根据任务自动识别并调用相关 skill。你也可以明确告诉 Claude 使用某个 skill。

### Q4：工作流铁律可以违反吗？

**A**：不可以。违反铁律会导致 fingerprint 失效、evidence 损坏、归档失败，必须废弃当前 change 重来。

---

## 八、总结

| 任务类型 | 使用方式 | 示例 |
|----------|----------|------|
| 新功能开发 | 工作流 + Skills | 添加新的监控指标 |
| Bug 修复 | 工作流 + Skills | 修复蓝牙连接问题 |
| 代码重构 | 工作流 + Skills | 统一内存管理 |
| 代码审查 | Skill | `/full-code-review` |
| 数据可视化 | Skill | `/dataviz` |
| 配置 Claude | Skill | `/update-config` |
| 运行测试 | 直接命令 | `ctest --test-dir build-x86/server` |
| 部署 | 直接命令 | `./tools/ci.sh` |

**记住**：
- **工作流** = 流程引擎（OpenSpec）
- **Skill** = 技术指南（superpowers）
- **两者配合使用，工作流优先**
