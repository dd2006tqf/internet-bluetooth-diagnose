# Workflow Rules — Agent 必读

本文档记录工作流中反复踩过的坑和必须遵守的规则。每次创建 change 前必须回顾。

## 铁律：禁止中途修改 harness 脚本

**任何情况下都不要在工作流运行期间修改 `scripts/` 目录下的任何文件。**

修改脚本 → 源指纹(source_fingerprint)变化 → 所有已记录证据的 `source_fingerprint_after` 与当前不匹配 → 全部证据失效 → 必须重来。

所有 harness 修改必须在创建 change 之前完成并提交。如果发现 harness bug，先归档或放弃当前 change，修完 harness 再开新 change。

## 完整操作顺序

```
# 规划阶段
1.  change_new.sh <name> --switch
2.  写 proposal.md / design.md / specs/ / tasks.md
3.  openspec_cli.sh validate <name> --strict        # spec 中的 requirement 必须包含 MUST/SHALL
4.  evaluator_check.sh --plan                        # 计划检查
5.  snapshot_update.sh --freeze-planning-baseline     # 冻结规划基线

# 实现阶段
6.  snapshot_update.sh --freeze-implementation-base   # 先冻结！（代码还没改）
7.  修改代码                                          # 现在改代码
8.  change_footprint.sh <name> --json                # 查看 footprint status
9.  task_verify.sh <id> --phase regression ...        # 记录证据
    - within_expected → 不加 --drift-reason
    - drift_warning 等 → 必须加 --drift-reason
10. task_verify.sh --complete <id>                    # 完成任务

# 评估阶段
11. integration_surface_check.sh <name> --refresh     # 刷新集成报告
12. sync_hashes.sh <name>                             # 同步哈希
13. evaluator_check.sh --begin                        # 开始评估
14. evaluator_check.sh --run ...                      # 运行评估命令
15. evaluation_template.sh <name>                     # 生成骨架
16. evaluation_fix.sh <name>                          # 自动填充可计算字段
17. pre_finish.sh <name>                              # finish 前一站式预检
18. evaluator_check.sh --finish                       # 完成评估

# 归档
19. change_archive.sh <name>                          # 归档
```

## 关键规则

### Design.md 不可修改
一旦开始用 task_verify.sh 记录 evidence，**绝对不能修改 design.md**。

修改 design.md → planning_fingerprint 和 tdd_policy_sha256 变化 → 所有已记录证据中的 planning_fingerprint_before/after 变成旧值 → "planning changed during command" → 所有任务无法完成。

- **实施前**：确认 design.md 的所有修改（classification、exceptions、thresholds）已完成并冻结
- **实施后**：只改代码文件和 tasks.md（勾选 checkbox），不碰 design.md
- **必须改时**：先运行 sync_hashes.sh 重新同步，然后对所有任务重新跑 task_verify.sh 记录证据

### Spec 格式
- requirement 描述必须包含 `MUST` 或 `SHALL`
- scenario 必须有 WHEN/THEN 内容，不能留空：
```markdown
#### Scenario: 场景名
- **WHEN** 触发条件
- **THEN** 预期结果
```

### Footprint 与 drift_reason
- `change_footprint.sh --json` 查看 status
- `within_expected` → task_verify **不加** `--drift-reason`
- `drift_warning` / `review_required` / `hard_exceeded` → **必须加** `--drift-reason`

### 冻结时机
- **先 freeze-implementation-base，再改代码**
- 如果反过来（先改代码再提交再 freeze），diff 为空，footprint 无变化

### pre_finish.sh
- `--finish` 之前必须运行 `pre_finish.sh`
- 它会自动完成：footprint 刷新 → integration 刷新 → hash 同步 → evaluation 自动填充 → 集成检查
- 全部通过后才运行 `--finish`

### manifest 哈希
- 修改任何 `scripts/` 文件后，运行 `manifest_sync.sh` 同步所有哈希
- 不要手动编辑 manifest.json 的 content_sha256

### 创建 change 前清理无关文件
创建 change 前，检查暂存区和工作区中是否有与本次 change 无关的脏文件。无关文件会被 evaluator 视为 "undeclared implementation paths" 导致 `--finish` 失败。

- **创建 change 前**：`git status` 检查，将无关文件先提交或 stash
- **不要用 `--adopt-path` 纳入无关文件**：adopted 路径会出现在 review_input 中但不在任何 task 的 `changed_paths` 里，evaluator 会拒绝
- **如果无关文件已经纳入**：在归档前将其从 adopted 路径移除，或在 evaluation.json 中添加 residual_risks 说明

### eBPF 变更必须从第一天就用 observability_only 例外
新增 eBPF 程序、日志、监控器等**纯可观测性功能**，在规划阶段就应该在 design.md 中声明 `observability_only` 例外，而不是等 evidence 失败后再补。

- 失败后再补例外 → 修改 design.md → planning_fingerprint 变化 → 已记录的 evidence 全废
- **规划时就决定**每个任务的验证方式（observability_only → alternative build）
- eBPF 编译要用容器流程（`docker exec weaknet-arm64-dev ... make`），开发机裸编译会报 `user_pt_regs` 错误

### 永远不要手工编辑 evidence 文件
`verification.json`、`evaluation.json`、`evaluation-baseline.json` 等 evidence 文件只能通过 harness 命令（`task_verify.sh`、`evaluator_check.sh`、`sync_hashes.sh`）维护。

- 手工 Write/Edit 很容易破坏 schema（例如 commands 数组嵌套）
- 发现坏数据时，用 harness 的正确流程重跑命令，不要手动改 JSON
- 错误结构一旦写入，`--complete` 校验会以 "TDD command schema mismatch" 拒绝

## 常见错误速查

| 症状 | 原因 | 解决 |
|------|------|------|
| `[ERR] footprint report missing, stale` | manifest 哈希不匹配 | `manifest_sync.sh` |
| `Error: stale source_fingerprint` | 中途改了脚本 | 不要改脚本！重来 |
| `[ERR] drift reason forbidden while within expected` | footprint 是 within_expected 但传了 --drift-reason | 去掉 --drift-reason |
| `[ERR] drift reason required for drift_warning` | footprint 是 drift_warning 但没传 --drift-reason | 加上 --drift-reason |
| `[ERR] managed Harness lock is held` | 上次运行残留锁 | `rm -rf .ai-harness/locks/managed-operation.lock` |
| `Error: specification_compliance requirements coverage mismatch` | evaluation.json 的 requirement_refs 为空 | `evaluation_fix.sh` 已修复，确保先运行它 |
| `Error: Project Command fingerprints do not match` | 改了脚本导致源指纹变化 | 不要改脚本！重来 |
| `Error: unapproved manifest ownership` | 新增脚本未注册到 templatePaths | 加脚本时同步更新 manifest_policy.js |
| OpenSpec archive 失败 | spec scenario 格式不对 | 确保 scenario 有 WHEN/THEN |
| `[ERR] lock owner purpose does not authorize` | 锁授权列表缺失组合 | 检查 harness_lock.sh 的授权列表 |
| `Error: planning changed during command` | 修改了 design.md 导致 planning_fingerprint 变化 | 不要改 design.md！重跑所有 evidence |
| `Error: undeclared implementation paths` | 脏文件不在任何 task 的 changed_paths 中 | 创建 change 前清理无关文件，或用 residual_risks 说明 |
| `Error: TDD command schema mismatch` | 手工编辑了 verification.json 导致结构损坏 | 不要手改 evidence！重跑 task_verify.sh |

## 工具速查

| 工具 | 用途 |
|------|------|
| `manifest_sync.sh` | 修改脚本后同步 manifest 哈希 |
| `pre_finish.sh <name>` | --finish 前一站式预检 |
| `evaluation_fix.sh <name>` | 自动填充 evaluation.json |
| `change_footprint.sh <name> --json` | 查看 footprint 状态 |
| `archive_recover.sh --status` | 查看归档恢复状态 |
