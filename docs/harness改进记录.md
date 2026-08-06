# Harness 工作流改进记录

记录 AutoAI-Harness 工作流在实际使用中发现的问题及修复。

---

## 改进点1：planning_fingerprint 哈希范围过大

### 问题

`planning_fingerprint` 对整个 `design.md` 文件做哈希。修改 `design.md` 的任何字段（如 Implementation Economy 分类配置、Integration Completeness 块），都会导致 `planning_fingerprint` 变化，使所有已记录的 TDD 证据失效（因为证据绑定了 `planning_fingerprint`）。

**实际影响**：改一个分类字段导致 10 个任务的 TDD 证据全部作废，需要重跑全部 RED→GREEN→REGRESSION。

### 修复

修改 `scripts/manifest_policy.js` 的 `planningStateAt` 函数：对 `design.md` 只哈希 TDD Policy 块（`<!-- autoai:tdd-policy:v1 -->` 之间的内容），不哈希整个文件。

修复后：
- 改 Implementation Economy 分类 → `planning_fingerprint` 不变 → TDD 证据不失效 ✅
- 改 Integration Completeness → `planning_fingerprint` 不变，`integration_completeness_sha256` 变化 → 只影响集成验收，不影响 TDD 证据 ✅
- 改 TDD Policy → `planning_fingerprint` 变化 → TDD 证据失效（正确行为）✅

### 修改文件

- `scripts/manifest_policy.js`（`planningStateAt` 函数的 `add` 回调）
- `.ai-harness/manifest.json`（同步更新 content_sha256）

---

## 改进点3：校验报错信息黑箱

### 问题

`integration_surface_check.sh` 报 `consumer_path_classification:<path>` 错误时，不告知：
- 当前的 `consumer_kind` 是什么
- 检测到的分类是什么
- 允许的分类有哪些
- 如何修复

用户只能翻源码才能理解错误原因。

### 修复

1. 修改 `scripts/integration_surface_lib.js` 第 542 行，报错信息增加 `consumer_kind`、`detected`（检测到的分类）、`allowed`（允许的分类列表）和修复建议。

2. 修改第 532 行，给 `downstream_build` 的 `allowedConsumerClasses` 增加 `production` 分类。原因：下游构建消费的编译产物（如 `server/bin/weaknet-dbus-server`）在 Implementation Economy 中被正确分类为 `production`，原配置不允许这个组合，导致误报。

### 修改文件

- `scripts/integration_surface_lib.js`（第 532、542 行）
- `.ai-harness/manifest.json`（同步更新 content_sha256）

---

## 改进点4：审计粒度太细，噪声太大

### 问题

每次命令执行生成一个 JSON 证据文件（`project-command-evidence/<hash>.json`），一个变更可能产生近百个文件。这些文件对项目本身无价值，只是 harness 自证负担。

### 修复

1. 新建 `scripts/audit_summary.sh`：读取 `verification.json`，生成人类可读的 `harness/audit-summary.md`，汇总每个任务的 TDD 闭环状态（RED/GREEN/REGRESSION 数量、是否闭环）。

2. 已通过 `.gitignore` 忽略 `project-command-evidence/`，不提交到远程。

### 用法

```bash
bash scripts/audit_summary.sh <change-name>
```

### 修改文件

- `scripts/audit_summary.sh`（新建）

---

## 改进点6：缺少逃生舱

### 问题

一旦进入工作流，中途想退出很难。变更卡在半路，要么硬走完，要么手动删目录。手动删除可能遗漏 selector 残留、harness 产物等。

### 修复

新建 `scripts/change_abort.sh`：优雅中止变更。
- 清理 harness 产物（verification.json、ai_snapshot.json、project-command-evidence/ 等）
- 清理 active-change selector
- 保留工作区源码改动（不碰 git 工作区）
- 可选 `--purge` 参数：同时删除 change 目录（含规划文档）

### 用法

```bash
# 仅清理 harness 产物，保留规划文档
bash scripts/change_abort.sh <change-name>

# 彻底删除 change 目录
bash scripts/change_abort.sh <change-name> --purge
```

### 修改文件

- `scripts/change_abort.sh`（新建）

---

## 改进点7：文档分散且重复

### 问题

`docs/ai/` 下有 13 个 .md 文件，内容有重叠，读者难以判断阅读顺序和各文档定位。`工作流使用指南.md`、`workflow.md`、`quick-brief.md` 三个都在讲流程，侧重点不同但容易混淆。

### 修复

在工作流使用指南中新增"文档导航"章节，明确每个文档的定位和阅读顺序，避免重复阅读。不删除 manifest 管理的模板文件（会破坏校验）。

### 修改文件

- `docs/ai/工作流使用指南.md`（新增文档导航章节）

---

## 改进点8：probe argv 契约不匹配（evaluation 阶段才暴露）

### 问题

`design.md` 中 surface 的 `evidence_contracts` 里 probe `argv` 写成了裸命令形式（如 `["make","-C","server"]`），但在 Generator/Evaluator 用 `--project-command build-server` 记录证据时，实际 argv 是包装器形式 `["scripts/project_command.sh","build-server","--change","<change>","--json"]`。`--plan-check` 阶段不校验 probe argv 与 runtime argv 匹配，只在 evaluation `--run` 阶段才报 `surface command does not match the approved probe contract`，导致返工。

### 修复（规则，非代码）

根据 `docs/ai/workflow.md` 的规则：
> "The command-ID form is recorded and matched as the normalized argv `scripts/project_command.sh <command-id> --change <active-change> --json`, so the Integration probe must declare that exact wrapper argv."

即：design.md 里 `evidence_contracts` 的 `argv`，只要是用 `--project-command` 记录证据的 surface，**必须**写 wrapper 形式，不能写裸命令。

### 教训

规划阶段写 `design.md` 之前，必须完整阅读 `docs/ai/workflow.md`（probe 契约）、`docs/ai/evaluation.md`（evaluation schema）。这个教训已写入 `CLAUDE.md` "Before any work" 节。

---

## 改进点9：纯编译验证的 surface 应设计为 build_or_install 类型

### 问题

本项目的 eBPF 运行时 + D-Bus 服务端需在 ARM64 开发板上运行，当前 VM/容器内无法独立跑行为验证。若 surface 设计为非 `build_or_install` 类型且 `verify_kinds` 只写 `["build"]`，`--plan-check` 会报 `surface needs test or behavior evidence`。

### 修复（规则，非代码）

涉及纯编译期接口变更（如新增成员、新增函数声明）、运行时需真机的 surface，应设计为 `build_or_install` 类型：
- `"kind": "build_or_install"`
- `"runnable_artifact": false`（无法运行真机时）
- `verify_kinds`: `["build"]`（build_or_install 类型允许只含 build）

这样避免强制要求 test/behavior 证据，同时用 TDD policy 的 `unavailable_hardware` 例外声明。

---

## 改进点汇总

| 改进点 | 问题 | 修复方式 | 状态 |
|---|---|---|---|
| 1 | fingerprint 哈希范围过大 | 只哈希 TDD Policy 块 | ✅ 已修复 |
| 3 | 校验报错黑箱 | 报错带允许值列表+修复建议；downstream_build 允许 production | ✅ 已修复 |
| 4 | 审计噪声太大 | 新建 audit_summary.sh 生成人类可读汇总 | ✅ 已修复 |
| 6 | 缺少逃生舱 | 新建 change_abort.sh 优雅中止 | ✅ 已修复 |
| 7 | 文档分散重复 | 使用指南新增文档导航 | ✅ 已修复 |
| 8 | probe argv 契约不匹配 | 规则：design 中必须写 wrapper argv | ✅ 已记录 |
| 9 | 纯编译 surface 验证 | 规则：设计为 build_or_install | ✅ 已记录 |

## 改进点10：freeze 前改代码导致 adopt 死锁

### 问题

正确顺序必须先 `freeze-implementation-base`（代码未改时），再改代码。若反序（先改代码再 freeze），freeze 时 `implementation_base_commit` 已写入，之后想 adopt 那些"已修改但未提交"的路径会报 `cannot adopt new paths after implementation starts`，形成死锁只能废弃 change。

### 教训

- 严格按工作流操作顺序：**先 freeze-implementation-base，再改代码**
- 若已先改代码，别尝试 adopt，直接废弃重来（`change_abort.sh --purge`），代码改动 commit 保留，重走工作流

---

## 改进点11：ai_snapshot 的 active_change 字段不能删除

### 问题

`change_abort.sh` 清理 selector 时若直接删掉 `active_change` 字段（而非设为 null），后续 `harness_active_optional`（依赖该字段存在）会静默返回 4，导致 `change_new` 等命令报奇怪的 exit 4 且无错误信息。

### 教训

`ai_snapshot.json` 必须保留 `active_change` 键（无活动 change 时为 `null`）。若发现缺失，补回 `"active_change": null` 并提交。

| 改进点 | 问题 | 修复方式 | 状态 |
|---|---|---|---|
| 10 | freeze 前改代码致 adopt 死锁 | 严格先 freeze 再改代码 | ✅ 已记录 |
| 11 | active_change 字段删除致 exit 4 | 保留该键为 null | ✅ 已记录 |
