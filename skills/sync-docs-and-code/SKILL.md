---
name: sync-docs-and-code
description: Use after modifying code to scan the entire repository for outdated documentation and obsolete code, syncing them to the latest state
---

# Sync Docs and Code (代码与文档同步更新)

## Overview

**Core principle:** Documentation and code must evolve together. A codebase with stale documentation or lingering obsolete code creates debt and misleads both developers and AI agents.

**Violating this process leaves silent discrepancies that cascade into bugs and planning errors.**

Per `CLAUDE.md`:
> “文档与代码同步：当代码修改与仓库现有文档相左（即文档落后于代码）时，必须顺带同步更新相应文档，保持文档与代码一致。”
> “Remove or account for obsolete code; do not leave parallel paths by default.” (Golden Principle #6)

---

## When to Use

**Run this skill:**
- After implementing or modifying any feature, bugfix, or refactoring
- When changing interfaces (C API, D-Bus methods/signals, config keys, CLI flags)
- When adding, removing, or renaming monitors, threads, or eBPF probes
- Before submitting a branch, creating a PR, or archiving an OpenSpec change
- Periodically to audit repository health and eliminate documentation drift

---

## The Four-Step Process

```
┌─────────────────┐     ┌─────────────────┐     ┌─────────────────┐     ┌─────────────────┐
│ 1. 差异分析      │ ──→ │ 2. 文档漂移扫描  │ ──→ │ 3. 过时代码排查  │ ──→ │ 4. 同步更新与验证│
│ Git Diff 提取实体│     │ 全局 Grep 比对  │     │ 孤儿接口/废弃宏  │     │ 批量修正+跑单测  │
└─────────────────┘     └─────────────────┘     └─────────────────┘     └─────────────────┘
```

### Step 1: 提取代码变更实体 (Diff & Entity Extraction)

检查当前分支与基线的改动范围，提炼关键变更点：

```bash
# 查看近期变动的核心文件与行数
git status --short
git diff HEAD~1 HEAD --stat # 或与主分支比较 git diff origin/main...HEAD --stat
```

**重点提取以下 6 类实体变更：**
1. **接口与签名**：C API（`weaknet_client.h`）、D-Bus 接口（`dbus_service.cpp` / XML）、CLI 命令参数
2. **架构指标与数量**：监控器个数（`MonitorManager`）、后台线程数（`ServerContext`）、eBPF 探针清单（`*.bpf.c`）
3. **配置项与常量**：`weaknet_config.hpp` 中的白名单配置、默认采样间隔、默认端口/路径
4. **存储与持久化**：SQLite 表结构、数据库文件路径（`history.db`）
5. **构建与依赖**：CMakeLists 编译选项、产物路径（如 `build-x86/`、`build-arm64/`）
6. **废弃旧方法**：被标记或替换掉的旧函数、旧常量

---

### Step 2: 全仓库检索过时文档 (Scan Stale Documentation)

根据 Step 1 提取的实体关键词，对项目全部文档进行全局正则扫描：

```bash
# 核心文档清单扫描
# 1. 架构与顶层说明：README.md, docs/架构设计.md, docs/项目评估.md
# 2. 客户端与集成说明：client/README_CLIENT.md, client/README_LIBRARY.md, docs/weaknet_cli_usage.md
# 3. 部署与环境说明：docs/交叉编译与开发板部署.md, docs/ci-测试使用指南.md
# 4. 技能与规范文档：CLAUDE.md, docs/skills-and-openspec-guide.md, skills/
```

**检索比对项清单：**
- [ ] **eBPF 程序清单**：统计当前实际 `.bpf.c` 数量（`find server/src -name "*.bpf.c"`），检查 `docs/架构设计.md` 等文档中的表格是否缺漏。
- [ ] **监控线程与插件清单**：检查文档中记录的监控器线程矩阵是否与 `server.cpp` / `monitor_registry.cpp` 实际启动项一致。
- [ ] **构建与测试指令**：检查所有 Markdown 中出现的编译测试命令是否包含旧路径（如陈旧的 `build/` 而不是 `build-x86/`），是否缺少新增的单测过滤项。
- [ ] **D-Bus 方法与信号**：核对文档中描述的 D-Bus 接口是否包含最新扩展（如 `GetEbpfMonitorHealth` 等）。
- [ ] **配置键名与类型**：核对文档中给出的 `weaknet-cli set/get` 示例参数是否与代码中的白名单匹配。
- [ ] **路径硬编码**：检查文档中是否存在违规过时的 `/tmp/weaknet/` 路径（应为持久化路径）。

---

### Step 3: 全仓库检索过时代码与孤儿接口 (Scan Obsolete & Dead Code)

检查是否有代码层面的“半废弃”残留：

1. **废弃函数与平行路径**：
   - 搜索已被新接口替换的旧函数是否还有生产调用方；
   - 坚决杜绝“为了兼容临时保留但无人维护”的平行实现。
2. **孤儿接口（Orphan Interfaces）**：
   - 检查 `server/src/` 中导出的非内部辅助函数是否有真实调用方（不能仅被单元测试调用）。
3. **未清理的旧注释与 TODO**：
   - 检查代码中是否残留已解决问题的旧注释、已被证伪的假设或过期的 FIXMEs。
4. **编译目标与构建脚本遗留**：
   - 检查 `CMakeLists.txt` 中是否包含已删除文件，或者编译产物名字是否与部署脚本脱节。

---

### Step 4: 同步更新与验证 (Update & Verify)

1. **就地修改**：
   - 保持“代码改动到哪里，文档就更新到哪里”的纪律；
   - 同步修正 Markdown 文档中的陈述、表格、命令参数、示例代码。
2. **运行全量测试验证**：
   任何死代码清理或文档示例更新后，必须执行全量验证，确保没有引发意外破坏：
   ```bash
   cmake --build build-x86 -j$(nproc) && \
   ctest --test-dir build-x86/server -R "test_net_info|test_quality|test_anomaly|test_audio|test_band|test_serializer|test_event|test_bt_full|test_bt_monitor$|test_iface|test_logger|test_traffic|test_database|test_weaknet_config|test_monitor" --output-on-failure
   ```
3. **原子性提交**：
   将代码改动与文档同步修改一并纳入提交，保持 Git 历史原子性（例如 `feat: xxx and update docs` 或单独 `docs: sync documentation with recent changes`）。
