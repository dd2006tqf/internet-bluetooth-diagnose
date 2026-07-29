# AutoAI-Harness 工作流使用指南

本项目集成了 AutoAI-Harness 工作流，用于对大型变更进行结构化管理。
本文档说明**你（项目作者）如何使用这套工作流**。

---

## 核心结论：全程与 AI 对话，不需要你敲命令

整个工作流是**通过与我（AI agent）对话完成的**。

- 你**不需要**在终端里敲任何命令
- 我会在需要时自动调用 `scripts/` 下的脚本
- 你只需要做三件事：**提需求 → 审批 → 归档**

---

## 什么时候用工作流？

| 场景 | 是否用工作流 |
|---|---|
| 新功能开发（如新增 WiFi 分析模块） | ✅ 用 |
| 重构核心架构（如改 D-Bus 通信层） | ✅ 用 |
| 跨多文件的大改动（>300 行） | ✅ 用 |
| 改 typo、改注释 | ❌ 不用 |
| 单文件小 bug 修复 | ❌ 不用 |
| 已完成的存量代码（如蓝牙监控） | ❌ 不用 |

**判断标准**：改动大、需要追溯、影响核心功能 → 用工作流。

---

## 完整流程（5 个阶段）

```
你提需求 → [1. Planner 规划] → 你审批 → [2. 冻结基线]
→ [3. Generator TDD 实现] → [4. Evaluator 验收]
→ 你说"归档" → [5. Archive 自动提交推送]
```

### 阶段 1：Planner 规划

**你要做的**：告诉我以下信息（自然语言即可，没有格式要求）

#### 必须提供

1. **变更名称**（kebab-case 格式）
   - 例：`wifi-analyzer`、`fix-dbus-leak`、`add-health-endpoint`

2. **变更意图**（你想做什么？解决什么问题？）
   - 例："我要加一个 WiFi 信号分析功能，能扫描周围 AP 并检测信道拥堵"
   - 例："修复 D-Bus 服务端退出时卡住的问题"
   - 例："给 API 加一个 /health 健康检查端点"

#### 可选提供（建议提供，能让规划更精准）

3. **约束条件**
   - 性能要求：如"内存占用 <2MB"、"响应时间 <5ms"
   - 兼容性要求：如"不能破坏现有 D-Bus 接口"、"要兼容 ARM64"
   - 平台限制：如"要在 CentOS Stream 上跑"

4. **相关文件**（你知道要改哪些文件就告诉我，不知道也没关系）
   - 例："主要改 server/src/weak_netmgr.cpp 和 server/include/common.hpp"
   - 如果不知道，我会自己去探索代码库

#### 怎么给我？

直接在对话里用自然语言说就行，例如：

> 帮我开一个工作流变更，名称叫 wifi-analyzer。
> 我想加一个 WiFi 信号分析功能，扫描周围 AP 并检测信道拥堵。
> 约束：内存占用 <2MB，不能破坏现有 D-Bus 接口。
> 相关文件我不太清楚，你自己看。

**我做什么**：
1. 探索代码库，理解现有架构
2. 执行 `bash scripts/change_new.sh <name>` 创建变更目录
3. 写 4 个规划文档：
   - `proposal.md` — 目标、范围、影响
   - `design.md` — 技术方案 + TDD 策略 + 集成完备性
   - `tasks.md` — 任务拆分（每个 task 有 Verify 种类）
   - `specs/<模块>/spec.md` — 需求规格（MUST 关键词 + WHEN/THEN 场景）
4. 运行校验：`openspec validate --strict` + `integration_surface_check.sh --plan-check`
5. 把文档内容展示给你 review

---

### 阶段 2：审批

**你要做的**：review 我写的 4 个文档，然后回复：

- 通过 → 回复"**审批通过**"
- 要改 → 回复"design.md 第 X 段改成 XXX"

**我做什么**：冻结规划基线（`planning_fingerprint`），进入实现阶段。

---

### 阶段 3：Generator TDD 实现

**你要做的**：通常**什么都不用做**。

两种情况需要你：
- **环境信息**：如果测试需要特殊硬件（如 eBPF、蓝牙设备），告诉我环境限制
- **决策点**：如果实现中遇到多个方案，我会问你选哪个

**我做什么**：对 `tasks.md` 里的每个 task，严格按 TDD 顺序：

```
RED          → 先写一个会失败的测试
GREEN        → 实现代码让测试通过
REGRESSION   → 跑全量测试确保没破坏其他功能
```

每步通过 `scripts/task_verify.sh` 记录证据到 `harness/verification.json`。

---

### 阶段 4：Evaluator 验收

**你要做的**：**什么都不用做**。这是独立验收环节。

**我做什么**：执行 `scripts/evaluator_check.sh --run`，独立验证：
- TDD 闭环是否完整
- 规划文档与实现是否一致
- 代码质量
- 无遗漏测试

产出 `evaluation.json`（Pass / Fail）。

- 如果 **Fail**，我回到阶段 3 补证据或修问题
- 如果 **Pass**，进入阶段 5

---

### 阶段 5：Archive 归档

**你要做的**：回复"**归档**"。

**我做什么**：执行 `scripts/auto_archive_push.sh <name>`，自动完成：
1. 变更从 `openspec/changes/<name>/` 移到 `openspec/changes/archive/<日期>-<name>/`
2. `git add` + `git commit` + `git push`
3. 远程仓库更新

---

## 你需要做的事汇总

| 阶段 | 你要做什么 | 怎么做 |
|---|---|---|
| 阶段 1 | 提供变更名称 + 意图 + （可选）约束/文件 | 对话里用自然语言告诉我 |
| 阶段 2 | review 文档，回复"审批通过"或修改意见 | 对话里回复 |
| 阶段 3 | 通常无需操作；特殊环境/决策点会问你 | 对话里回复 |
| 阶段 4 | 无需操作 | — |
| 阶段 5 | 回复"归档" | 对话里回复 |

**全程在对话里完成，不需要你敲终端命令。**

---

## 示例：一次完整对话

**你**：帮我开一个工作流变更，名称叫 add-health-endpoint。
我想给 API 加一个 /health 健康检查端点，返回服务状态。
约束：响应时间 <5ms，不能破坏现有接口。

**我**：（探索代码库，创建变更，写 4 个文档，展示给你）
> 规划文档已写好，请 review：
> - proposal.md：目标是新增 /health 端点...
> - tasks.md：拆成 3 个 task...
> - specs/：需求规格...

**你**：审批通过

**我**：（按 TDD 实现 3 个 task，每个跑 RED→GREEN→REGRESSION）
> T1 完成，T2 完成，T3 完成。进入验收。

**我**：（跑 Evaluator）
> 验收 Pass。

**你**：归档

**我**：（执行 auto_archive_push.sh）
> 已归档并推送到远程。完成。

---

## 命令速查（供参考，通常不需要你手动执行）

| 步骤 | 命令 |
|---|---|
| 创建变更 | `bash scripts/change_new.sh <name>` |
| 查看变更状态 | `bash scripts/change_status.sh <name>` |
| 记录 TDD 证据 | `bash scripts/task_verify.sh <id> --phase <p> --kind <k> --project-command test-all --expect-exit <0\|1>` |
| Evaluator 验收 | `bash scripts/evaluator_check.sh --run` |
| 归档并推送 | `bash scripts/auto_archive_push.sh <name>` |
| 规划校验 | `openspec validate --strict` |
| 集成完备性检查 | `bash scripts/integration_surface_check.sh --plan-check` |

---

## 注意事项

1. **已完成的存量代码不走工作流**（如蓝牙监控）
2. **审计记录不提交远程**（已配置 .gitignore 忽略 `project-command-evidence/`）
3. **一个工作目录只有一个 writer**，不要同时开两个变更
4. **变更名称用 kebab-case**（小写字母 + 连字符，如 `wifi-analyzer`）
