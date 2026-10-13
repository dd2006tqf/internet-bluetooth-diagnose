# 工作流错误速查与工具参考

> 本文档原为 `workflow-rules.md`（铁律与关键规则），铁律已全部迁移至 `CLAUDE.md` 的 Hard rules 节。
> 本文档保留为**参考文档**，仅包含常见错误速查表和工具使用说明。
> 所有 Agent 必读的铁律请参见 `CLAUDE.md`。

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
| `Error: duplicate main requirement` | 主 spec 中有重复 requirement 名称 | 先 grep 确认不存在，再添加 |
| `[ERR] footprint 状态为 review_required` | 实际代码量超出 design 的 review_at 阈值 | 调高 threshold 或接受 drift_warning |
| `Error: cannot adopt new paths after implementation starts` | freeze 后尝试 adopt 新路径 | 废弃 change 重来，先 freeze 再改代码 |
| `Error: surface command does not match the approved probe contract` | probe argv 不是 wrapper 形式 | design 中必须写 `["scripts/project_command.sh", ...]` |
| `Error: surface needs test or behavior evidence` | surface 非 build_or_install 但 verify_kinds 只有 build | 改为 build_or_install.runnable_artifact=false 或用 exception |
| `Error: task must be uniquely unchecked` | tasks.md 中多个 task 有相同编号 | 检查 task 编号唯一性 |
| `Error: completed task has no prior evidence` | task 标记为已勾选但无 evidence | 先记录 evidence 再 --complete |
| `Error: requirements coverage mismatch` | task Covers 未覆盖所有 delta 场景 | 每个 spec 场景至少有一个 task Covers |
| `Error: tasks do not cover the complete delta universe` | task Covers 的 scenario 名字与 spec 不匹配 | 确保 Covers 场景名与 spec 完全一致（含空格） |
| `Error: footprint 状态为 invalid (unclassified paths)` | 改动文件未列入 design 的 classification | 将文件加入 production/tests 分类 |
| `Error: pre-existing dirty paths need explicit --adopt-path` | freeze 时工作区有脏文件 | 用 --adopt-path 声明，或先提交再 freeze |
| `Error: adopted path is not a pre-existing dirty path` | 代码已提交故不是脏路径 | 代码已提交则不需要 adopt，工作区只留真正脏的 |
| `Error: local snapshot schema (active_change 缺失)` | ai_snapshot.json 缺少 active_change 字段 | 补回 `"active_change": null` |
| `Error: integration surface report missing` | 未运行 `--refresh` | `integration_surface_check.sh <change> --refresh --json` |
| `Error: footprint 状态为 invalid (footprint check FAIL)` | design 的 threshold 不合理致超出 hard_limit | 调高 threshold 或缩减变更范围 |

## 工具速查

| 工具 | 用途 |
|------|------|
| `manifest_sync.sh` | 修改脚本后同步 manifest 哈希 |
| `pre_finish.sh <name>` | --finish 前一站式预检 |
| `evaluation_fix.sh <name>` | 自动填充 evaluation.json |
| `change_footprint.sh <name> --json` | 查看 footprint 状态 |
| `archive_recover.sh --status` | 查看归档恢复状态 |
| `change_abort.sh <name> [--purge]` | 优雅中止变更（可选删除 change 目录） |
| `audit_summary.sh <name>` | 生成 TDD 闭环状态汇总 |
| `sync_hashes.sh <name>` | 同步所有哈希 |
| `evaluation_template.sh <name>` | 生成 evaluation.json 骨架 |
| `integration_surface_check.sh <name> --refresh --json` | 刷新集成表面报告 |