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

## 工具速查

| 工具 | 用途 |
|------|------|
| `manifest_sync.sh` | 修改脚本后同步 manifest 哈希 |
| `pre_finish.sh <name>` | --finish 前一站式预检 |
| `evaluation_fix.sh <name>` | 自动填充 evaluation.json |
| `change_footprint.sh <name> --json` | 查看 footprint 状态 |
| `archive_recover.sh --status` | 查看归档恢复状态 |
