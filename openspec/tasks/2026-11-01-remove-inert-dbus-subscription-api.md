# 删除失效的 D-Bus 订阅族（full-code-review O1 结项）

> 日期：2026-11-01
> 分类：工作流外实施（承接 [[2026-11-01-ota-convergence-and-orphan-interface-fixes]] 的遗留 3）
> 结果：已完成（8 个导出符号已删除，全部消费者同步清理，本机全量验证通过）

## 构思

### 要解决什么

`weaknet_client.h` 里有一族事件订阅 API，自服务端 2026-10-19 停发全部
D-Bus 信号后长期处于**空转**状态：

| API | 实际行为 |
|---|---|
| `weaknet_check_changes()` | 永远返回 false |
| `weaknet_subscribe_event()` | 只加 D-Bus match，服务端不发 |
| `weaknet_unsubscribe_event()` | 只改本地状态，固定返回 true |
| `weaknet_get_event_types()` | 本地常量拼接，与生产端无关 |
| `weaknet_check_events()` | 永远返回 false |
| `weaknet_subscribe_network_quality()` | **阻塞**消息循环里，永不返回事件 |
| `weaknet_check_network_quality()` | 永远返回 false |
| `weaknet_subscribe_bluetooth_events()` | 只加 match，收不到信号 |

上一批次（O1）已把它们标注为 `@deprecated` 并在文件头写明失效，但**没有删除**。
遗留判断是「删除会牵动 `test_client.cpp`、`test_network_quality.cpp` 与
`libweaknet.so` 的 ABI，属独立变更；且删除还是重建发射需要产品决策」。

### 为什么这次可以删

用户明确决策：**选 A（删除）**。

判断依据是「哪种错误更危险」：

- 保留 → 集成方看到函数返回 `true`，以为订阅成功，据此设计事件驱动架构，
  上线后才发现永远收不到事件。**静默的谎**。
- 删除 → 编译期直接报错，集成方被迫改用 `get_*` 查询。

编译错误远好于运行期的静默失效。且「重建发射」在当前不成立——**没有任何
真实订阅者**，先建发射端等于凭空造孤儿接口，违反项目铁律。

## 实现方式

### 删除范围

| 文件 | 改动 |
|------|------|
| `client/client.cpp` | 删除 8 个 `extern "C"` 函数实现（**175 行**，行 1401–1783 区间内 8 段） |
| `client/weaknet_client.h` | 删除 10 段：8 个函数声明 + 2 个回调 typedef（`weaknet_event_callback_t` / `weaknet_network_quality_callback_t`，约 **114 行**）；清理因此变空的 3 个节标题；文件头说明由「暂留」改写为「已删除 + 迁移指引」 |
| `client/test_client.cpp` | 删除 6 个订阅测试用例 + 12 个 CLI 子命令 + 对应帮助文本与文件头信号列表（**1073 → 691 行**）；`test-bt` 保留设备/适配器查询，仅去掉事件监听调用 |
| `client/test_network_quality.cpp` | **整文件删除**（唯一用途就是演示已删接口） |
| `client/example_usage.cpp` | 删除无引用的 `onNetworkChange` 回调及其注释 |
| `client/CMakeLists.txt` | 移除 `test_network_quality` 构建目标（留注释说明原因） |
| `client/README_LIBRARY.md` | 订阅示例改为轮询示例；「事件系统/状态监控」API 小节改写为删除说明 + 迁移指引 |
| `docs/架构设计-模块与代码结构.md` | 第 2、3 小节（信号订阅 / 非阻塞检查）整段改写为删除记录；目录树移除已删文件；架构图注更新 |

### ABI 影响（**破坏性变更**）

仓库当前**无 SONAME、无版本脚本**（`client/CMakeLists.txt` 只有
`OUTPUT_NAME weaknet`）。移除 8 个导出符号后：

- 旧二进制运行时会出现 `undefined symbol` 而**加载失败**（不会静默降级）；
- 仓库内全部消费者已同步清理，无遗留。

### 关键决策

1. **一次性彻底删，不留兼容壳**。留空实现只会延续「返回成功但无效果」的
   谎言，违背删除初衷。
2. **`test-bt` 半保留**。它组合了「设备列表 + 适配器 + 事件监听」三件事，
   前两者是真实可用的查询，只删事件监听那一次调用。
3. **不顺手修既有警告**。`test_client.cpp:124` 的 `unused variable 'error'`
   在删除前就存在（已比对备份确认），属无关改动，按「最小独立可验证闭包」
   铁律不碰。

### 本机验证

```
x86 全新编译（rm -rf build-x86）         → EXIT=0，无 error
ctest --test-dir build-x86/server         → 54/54 通过
nm -D libweaknet.so                       → 37 个 weaknet_* 导出符号
8 个目标符号                              → 逐一确认已删除
test_client_bin（帮助输出）               → 无失效子命令
test_client_bin get                       → 正常运行
OTA 单测 / 三套演练                       → 55 / 44 / 30 / 36 全绿（确认未受影响）
```

## 结果

### 已完成

- 8 个空转 API 从库、头文件、示例、测试、文档中彻底移除
- ABI 破坏性变更已在文档中如实标注（无 SONAME，旧二进制会硬失败）
- 迁移路径明确：改用 `get_*` / `*_stats` 轮询查询

### 遗留

- **`libweaknet.so` 仍无 SONAME**：本次删除让「无版本控制」的代价显性化。
  若后续还有 ABI 变更，建议补 `SOVERSION` + 符号版本脚本，让旧二进制至少
  能给出「需要 v2」而非裸 `undefined symbol`。属独立变更。
- **板端尚未同步**：本次只做了本机验证，需 `./tools/ci.sh` 部署后才生效
  （板端现有二进制仍链接旧库，但仓库内消费者已全部改过，不会出问题）。

### 后续方向

- 若将来确实需要事件推送：**先有真实订阅者**，再走 OpenSpec change 重建
  服务端发射端。不得凭空恢复发射。
