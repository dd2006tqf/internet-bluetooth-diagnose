# 文档计数与构建路径漂移订正

> 日期：2026-11-06
> 分类：工作流外实施 / 文档与代码同步
> 结果：已完成（4 个文档订正；实测基线与代码逐项对齐；顺带修复 harness 阻塞项）

## 构思

### 触发原因

收尾 LRADC 驱动工作时按 CLAUDE.md「文档与代码同步」条款复核仓库文档，
用真实命令而非文档自述去对账，发现三类系统性漂移。它们都不是笔误，
而是**代码演进了、文档停在旧口径**：

| 类别 | 漂移 | 危害 |
| --- | --- | --- |
| 测试套件数 | 文档写 48 / 53 / 55，实测 `ctest -N` = **56** | 读者按文档判断覆盖率，实际少算 8 个套件 |
| 测试表格缺口 | 表格 52 行，实际 56 个套件 | 4 个真实套件在文档里不存在，等于隐形测试 |
| eBPF 程序数 | 文档写 11，实测 `.bpf.o` = **12** | 简历/汇报口径错误 |
| 构建路径 | §十二、§二 仍教 QEMU 容器 + `-j1` | 与 §一「原生交叉编译 ~40 秒」自相矛盾，新人会走已废弃的 15 分钟路径 |

### 为什么必须先取实测基线

`grep -c 'add_test('` 只返回 2，看起来"没测试"；真实注册方式是
`server/test/CMakeLists.txt` 里的 `foreach(test_target ...) add_test(...)`
循环，加上条件注册的 `test_ebpf`。所以唯一可信的来源是
`ctest --test-dir build-x86/server -N`。同理 eBPF 数量不能数源码目录
（`client/ebpf/` 下没有 `.bpf.c`），要数构建产物
`find build-arm64-native -name '*.bpf.o'`。

**先用可复算命令取基线，再改文档**，否则改完仍不知道对不对。

### 为什么不做进 OpenSpec change

这是文档订正，不产生新的产品表面（接口/入口/配置键/协议/构建安装面），
没有消费者也没有可观测命令。按孤儿判定，它不满足 change 的前提。
按 CLAUDE.md「工作流外任务必须留记录」的要求，记在本文件。

## 实现方式

### 基线采集（全部实测，命令可复跑）

```bash
ctest --test-dir build-x86/server -N | tail -2        # → Total Tests: 56
find build-arm64-native -name '*.bpf.o' | wc -l       # → 12
wc -l kmod/weaknet_lradc_keys.c                       # → 1362
git diff --stat -- kmod/README.md                     # → 156 insertions(+), 4 deletions(-)
```

套件名逐条导出后与文档表格做集合差分：

```bash
ctest --test-dir build-x86/server -N | sed -n 's/^  Test *#[0-9]*: //p' | sort
```

差分结果：表格缺 4 个（`test_client_cli_contract_gtest`、
`test_looper_dispatch_gtest`、`test_observer_attribution_gtest`、
`test_update_status_gtest`），**过时 0 个**。补表前逐个读了测试文件头注释
与 `TEST`/`TEST_F` 宏，按真实覆盖内容写描述，不臆测。

### 逐项改动

| 文件 | 改动 |
| --- | --- |
| `CLAUDE.md` | 242 行套件数 53 → 56；注意事项段把"eBPF 必须在 ARM64 容器内编译 / 必须 `-j1` / QEMU 全量 10-20 分钟"改为原生交叉编译为主、旧容器仅兜底 |
| `docs/ci-测试使用指南.md` | 三处 55 → 56（第 24、53、108 行）；表格补 4 行至 56 行 |
| `docs/skills-and-openspec-guide.md` | 535 行 48 → 56；§4.1 容器编译命令换成原生交叉编译；§4.4 `docker ps` 换成 eBPF 产物计数；§6.4 重写并行度说明 |
| `docs/交叉编译与开发板部署.md` | 9 / 26 / 76 行 11 → 12；28 行 48 → 56；§二 构建产物映射全部换成 `build-arm64-native` + `-j$(nproc)`；§十二「编译性能/编译规则/ccache/产物验证/手动编译步骤」整段按原生路径重写 |
| `openspec/tasks/2026-11-06-lradc-keys-driver.md` | 行数 1425 → 1362；README 改动 +111 → +156/-4，标题补「最终判定」 |

### 关键判断：哪些"旧数字"不改

复核时发现两类看似过时、实则正确的表述，**刻意保留**：

1. `docs/交叉编译与开发板部署.md:683` 的「必须使用 `-j1`」位于
   「cc1 崩溃（QEMU 并行度过高）」故障排查小节，是历史故障的正确处置说明；
2. `docs/网关动作目录版本契约.md:122` 的「47 → 48 套件」是
   2026-10-08 验收记录里的事实陈述，改成 56 会篡改历史验收证据。

> 判据：**描述当前状态的要改，记录历史事实的不改。** 否则等于伪造记录。

## 结果

### 已验证

| 检查 | 结果 |
| --- | --- |
| 套件表格与 ctest 集合差分 | 表格 56 / ctest 56，缺失 none，过时 none |
| 残留 53/55/48 套件声明 | 仅剩 2 处历史记录（见上），当前指南已清零 |
| 残留 11 个 eBPF 声明 | 当前文档清零，仅 `openspec/tasks/2026-11-05-*` 历史记录保留 |
| 残留无条件「必须 `-j1`」 | 仅剩故障排查小节与历史验收记录，均正确 |
| 文档改动量 | 4 文件，+72 / -63 |

### 附带修复：harness 阻塞项（已闭环）

复核时发现一个**会阻断下一次 OpenSpec change** 的问题：

```text
loadManifest FAILED: managed template content mismatch: CLAUDE.md
harness_doctor --json → 32 checks, 31 pass, 1 fail (control.manifest)
内容漂移: CLAUDE.md  (recorded e964c46d… → actual 09295438…)
文件缺失: init.sh    (2a80f6b 已删除，但仍登记在 templatePaths 与 manifest.json)
```

任何调用 `loadManifest()` 的包装器都会被它挡住（`snapshot_update.sh`、
`evaluator_check.sh`、`evaluation_template.sh`、`evaluation_fix.sh`、
`openspec_preflight.sh`、`change_scope.js`、`harness_doctor.js`），
即**下一个 change 无法创建、无法记录证据**。

按选项 A 修复（不复活 `init.sh`——它是被有意删除的孤儿，复活违反孤儿判定）：

1. `scripts/manifest_policy.js` 的 `templatePaths` 移除 `init.sh`；
2. `.ai-harness/manifest.json` 的 `managed_paths` 同步移除该条目；
3. `scripts/manifest_sync.sh` 刷新 `CLAUDE.md` 与 `manifest_policy.js` 两条哈希。

复验（2026-11-06）：

```text
node -e 'loadManifest()'          → OK, upgrade_required = false
bash scripts/workflow_contract_check.sh → OK
82 条 template 条目全量复核        → 内容漂移 0 / 文件缺失 0
```

详细过程与教训见 `docs/harness改进记录.md` 改进点13。

### 顺带发现的板端阻塞

复验 LRADC 实测结论时板端不可达：

```text
ssh board → Connection timed out during banner exchange
127.0.0.1:2222 仍在 LISTEN（反向隧道进程存活），但握手无响应
```

即隧道**本地半段在、远端半段已死**。按约定不擅自重启隧道服务，
LRADC 的板端复验留待隧道恢复后进行（此前记录的实测证据不受影响）。
