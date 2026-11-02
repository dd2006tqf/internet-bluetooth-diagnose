# 冒烟脚本假绿灯与 CLI 退出码缺陷（O1 删除的连带修复）

> 日期：2026-11-02
> 分类：工作流外实施（缺陷修复 + 回归加固，承接 [[2026-11-01-remove-inert-dbus-subscription-api]] 的验证阶段）
> 结果：已完成。2 个真实缺陷已修，新增 1 个 gtest 契约测试固化，本机 56/56 全绿。

## 构思

### 怎么发现的

`2026-11-01-remove-inert-dbus-subscription-api` 的任务记录里写着「板端尚未同步，
需 `./tools/ci.sh` 部署后才生效」。部署前按惯例先做一次全仓残留引用扫描，
本意只是确认文档干净，结果扫出两处**功能性**问题（不是文档问题）：

1. `tools/weaknet-test-full.sh`（板端冒烟脚本）仍在调用 4 个已删除的子命令：
   `test-events`、`test-quality`、`check`、`event-types`；
2. `test_client_bin` 对**未知子命令返回退出码 0**。

两者叠加的后果比单独任何一个都严重：冒烟脚本的 `run_client_test()` 用
`if ./client/bin/test-client $cmd; then ok; else fail; fi` 判定，而未知命令
返回 0 会让它走进 `ok` 分支 —— **删掉的命令被判成 PASS**。

这意味着上一批次报告的「板端冒烟 26 PASS / 0 FAIL」里，
那 4 项是**假绿灯**，不是真通过。

### 为什么这是必须修的真缺陷

- 若只修脚本不修退出码：下次再有子命令被删，同样的问题会重演，而且
  依旧无声。脚本与实现是两份独立维护的清单，靠人工同步必然漂移。
- 若只修退出码不修脚本：本次部署会立刻从「假 PASS」变成 4 个真 FAIL，
  ci.sh 直接红。两种都是错的。
- 必须先修退出码（让失败可见），再修脚本（让清单正确），最后加契约测试
  （让漂移在 CI 就被发现，而不是等板端冒烟）。

### 为什么不走 OpenSpec change

改动面是「一个 CLI 的退出码语义 + 一个测试脚本的调用清单 + 一个静态契约
测试」，不新增产品 surface、不改 D-Bus 协议、不改云端契约、不改配置项、
不改构建图。按 `docs/ai/implementation-economy.md` 判据属工作流外实施。

## 实现方式

### 缺陷 1：`main()` 丢弃 `runSingleTest()` 的返回值

`client/test_client.cpp` 原代码：

```cpp
if (command == "all") {
    runAllTests();
} else {
    runSingleTest(command, argc, argv);   // 返回值被丢弃
}
return 0;                                  // 无论成败都返回 0
```

修法（最小改动，不动 `runSingleTest` 内部——它的 22 个分支本来就都正确
`return false`）：

```cpp
if (command == "all") {
    runAllTests();
} else if (!runSingleTest(command, argc, argv)) {
    return 1;
}
return 0;
```

这一改同时修好两件事：未知子命令返回非零；D-Bus 查询真实失败（如服务端
未启动）也返回非零。此前两种情况都是 0。

### 缺陷 2：冒烟脚本调用已删除的子命令

`tools/weaknet-test-full.sh`：

- 删掉 Phase 5（事件系统 `test-events`）与 Phase 6（网络质量 `test-quality`）
  两个整段，改写为说明性注释；
- 删掉 Phase 10 里的 `run_client_test "check"` 与 `run_client_test "event-types"`；
- **保留 Phase 编号不重排**（4 → 7），因为日志、文档与历史排障记录都按
  这些编号引用，重排的收益小于打乱引用的代价；
- 顺带把注释里的「39 个 gtest 套件」更正为 55。

### 顺带的文档同步

`client/README_CLIENT.md` 与 `client/README_LIBRARY.md` 里还有大量把已删
接口当可用接口介绍的内容，一并修正：

| 文件 | 问题 | 处理 |
|---|---|---|
| `client/README_CLIENT.md` | 命令行示例列了 `check` / `subscribe` / `file` / `bt-events`（其中 `file` 与订阅无关，是更早的遗留） | 删除这 4 行；补「变更说明」段 |
| `client/README_CLIENT.md` | 「网络状态变化信号」整节仍在描述服务端行为 | 改写为「已停发」+ 历史对照表 + 轮询迁移指引 |
| `client/README_CLIENT.md` | 特性列表「📡 实时监控网络状态变化」 | 改为「周期性查询网络状态」 |
| `client/README_CLIENT.md` | C 接口函数列表与 C++ 示例仍含 `weaknet_check_changes` | 删除 |
| `client/README_LIBRARY.md` | 测试段示例调用 `test-events` 两次 | 删除，补退出码说明 |
| `client/README_LIBRARY.md` | 「支持的事件类型」整节 | 改为「已停发」+ 历史对照 |

`docs/架构设计-模块与代码结构.md` 里出现的同名 API 属于**删除记录**（第 2 小节
标题即「已于 2026-11-01 删除」），表述正确，不需改动。

### 新增回归测试

`server/test/unit/test_client_cli_contract_gtest.cpp`，3 个用例，
纯静态分析（读源码文本），不需要 D-Bus、不需要板端，x86 与 CI 直接跑：

| 用例 | 固化的契约 |
|---|---|
| `EverySmokeScriptCommandIsImplemented` | 冒烟脚本里 `run_client_test "<name>" "<cmd>"` 调用的每个 cmd，必须在 `runSingleTest()` 中有对应的 `command == "<cmd>"` 分支 |
| `MainPropagatesSingleTestFailure` | `main()` 必须检查 `runSingleTest()` 的返回值并返回非零 |
| `RemovedSubscriptionCommandsStayRemoved` | 11 个已删除的订阅族子命令不得重新出现在冒烟脚本里 |

写法照抄仓库已有的同类先例 `test_map_sizing_spec_sync_gtest.cpp`（静态读源码做
交叉校验），`WEAKNET_REPO_SOURCE_DIR` 由 CMake 注入。

### 变异验证（证明测试不是空转）

| 变异 | 预期 | 实测 |
|---|---|---|
| 把 `main()` 改回 `runSingleTest(...); return 0;` | `MainPropagatesSingleTestFailure` 变红 | ✅ 1 FAILED |
| 把 `run_client_test "check" "check"` 加回脚本 | `EverySmokeScriptCommandIsImplemented` + `RemovedSubscriptionCommandsStayRemoved` 变红 | ✅ 2 FAILED |

两次变异均恢复原状后复跑全绿。

### 验证

```
x86 全量编译                                  EXIT=0（仅 1 条既有 unused-variable 警告）
ctest --test-dir build-x86                    56/56 通过
test_client_bin test-events / test-quality
              / check / event-types           EXIT=1（此前全为 0）
test_client_bin（无参，打印帮助）              EXIT=1（原有行为，未变）
bash -n tools/weaknet-test-full.sh             语法 OK
冒烟脚本 16 个子命令 vs 实现分支               无未实现残留
```

## 结果

### 已完成

- `main()` 退出码缺陷修复，未知子命令与查询失败均能传播为 `EXIT=1`
- 冒烟脚本移除 4 个已删除子命令，板端冒烟不再有假绿灯
- 新增 3 用例契约测试，把「脚本清单 ↔ 实现」的一致性固化进 CI
- `README_CLIENT.md` / `README_LIBRARY.md` 的中文文档与代码同步
- 套件计数同步：`README.md` 54→56（全量）、`docs/ci-测试使用指南.md`
  54→55（server 子目录）、冒烟脚本注释 39→55

### 一个方法论收获

上一批次的「板端冒烟 26 PASS / 0 FAIL」结论**不能作为删除正确性的证据**，
因为其中 4 项是假绿灯。这不是那次工作的错——缺陷藏在一个「返回值被丢弃」
的写法里，只有对照脚本调用清单才能发现。教训是：**脚本依赖退出码时，
必须先验证被调程序的失败真的会变成非零退出码**，否则所有基于 `if cmd; then`
的判定都不可信。

### 遗留

- **板端尚未部署**：本轮全部是本机验证。板端仍跑旧二进制，需 `./tools/ci.sh`
  部署后新行为才生效。部署后预期冒烟 PASS 数会比之前少 4（因为不再有那 4 项
  假绿灯），这是正确结果，不是回退。
- **`client/test_client.cpp:124` 的 `unused variable 'error'`** 是既有警告，
  与本次无关，按「最小独立可验证闭包」铁律未动。
