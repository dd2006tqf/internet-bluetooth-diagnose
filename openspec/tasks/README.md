# 工作流外任务记录（openspec/tasks/）

> 本目录记录**不走 OpenSpec change 工作流**的每一次任务（快速修复、spike、
> 探索性改动、工作流外实施等）。走工作流的任务由 `openspec/changes/` 管理，不放这里。

## 规则

1. **不走工作流 = 必须留记录**：任何不经 `change_new.sh` → 归档流程完成的任务，
   都要在本目录建一个文件，存放它的构思、实现方式和结果。
2. **一个任务一个文件**：命名 `YYYY-MM-DD-<kebab-name>.md`（与归档 change 的
   日期前缀风格一致）。
3. **不使用 harness 证据机制**：本目录不受 OpenSpec 铁律与 fingerprint 约束，
   可自由编辑；但也因此**不具备验收效力**——不能作为行为变更已验证的依据。
4. **三节结构**（可以加节，不可缺节）：

```markdown
# <任务名>

> 日期：YYYY-MM-DD
> 分类：快速修复 / spike / 探索 / 工作流外实施
> 结果：已完成 / 部分完成 / 已放弃

## 构思
要解决什么问题、为什么不做进 OpenSpec change、备选方案与取舍。

## 实现方式
改了哪些文件、关键决策、验证手段（编译/测试/真机命令）。

## 结果
最终状态、遗留问题、后续是否值得升格为正式 OpenSpec change。
```

## 与其它目录的分工

| 目录 | 定位 |
|---|---|
| `openspec/changes/` | 走完整工作流的变更（Planner → Generator → Evaluator → 归档） |
| `openspec/tasks/`（本目录） | 不走工作流的任务记录 |
| `docs/ideas/` | 尚未动工的设想（孵化区） |
