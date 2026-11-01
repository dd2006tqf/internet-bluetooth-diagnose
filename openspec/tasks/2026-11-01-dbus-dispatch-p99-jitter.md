# D-Bus 派发层 P99 抖动修复（单次派发与只看 POLLIN 的双重缺陷）

> 日期：2026-11-01
> 分类：工作流外实施（快速修复 + 回归加固）
> 结果：已完成。x86 单测 54/54、ARM64 交叉编译、板端冒烟 26 PASS / 0 FAIL、
> 板端延迟与干扰基线全部复测通过。

## 构思

### 要解决什么

服务端 D-Bus 方法调用出现**离散阶梯式**延迟：同一个 `GetInterfaces`，
最快 11ms、中位 160ms、最坏 **25 秒**，相差 2000 倍以上。而该方法体只有
两行（取快照 + 回数组），不可能耗时秒级。

调查路径（依次排除）：

| 假设 | 证据 | 结论 |
|---|---|---|
| CPU 争抢 | cyclictest 满载最大 748µs，与空载同级 | ❌ |
| 调度停顿 | 各线程 10s 内非自愿切换仅 1-3 次 | ❌ |
| D-Bus daemon 慢 | 直连 daemon 9-16ms 稳定，其他服务 12-15ms 稳定 | ❌ |
| 主线程被锁阻塞 | 896/900 次采样停在 `do_sys_poll` | ❌ |
| 客户端超时 | `--reply-timeout=10000` 仍出 3053ms，退出码全 0 | ❌ |

真正证据来自**日志时间戳对齐**：把每次调用的实测延迟与
`handleListInterfaces` 打印的时间戳逐条对比，间隔完全吻合
（1219ms↔1219ms、387ms↔394ms…）。这说明**延迟全部产生在
`handleXxx` 被调用之前**，即消息在派发层排队，处理逻辑本身耗时近零。

### 根因：两个叠加的错误假设

`server/src/looper.cpp` 原主循环：

```cpp
if (fds[0].revents & POLLIN) {
    (void)dbus_connection_read_write_dispatch(conn_, 0);   // 每次只派发一轮
}
```

注释声称"若仍有数据到达，poll 会立即再次唤醒"。这个推断有两半都不成立，
本次用两个一次性探针实测证伪：

1. **`read_write_dispatch` 一次只派发一条消息**。队列积压 8 条时调用一次，
   回调计数只 +1，`get_dispatch_status()` 仍返回 `DATA_REMAINS`。
2. **内部队列有积压但 socket 已读空时，`poll()` 不再报告 POLLIN**。
   实测：队列仍有余量时 `poll(fd, POLLIN, 300ms)` 返回 0（超时）。

叠加后果：积压期间主线程一直睡到 100ms 超时；醒来后若判定为"超时无事件"
就直接 `continue`，**连一次派发都不做**。积压只能等下一条 socket 数据
才被推进——这正是 13ms / 300ms / 1000ms / 3000ms 阶梯的来源，
因为阶梯步长就是 poll 周期与消息到达节奏的拍频。

### 为什么不做进 OpenSpec change

- 改动面是单个内部函数的派发语义，不新增对外 D-Bus 协议、不改云端契约、
  不改配置项、不改构建图；
- 缺陷修复类工作按 `docs/ai/implementation-economy.md` 判据走工作流外实施
  并在此留记录，与既有同类修复（OTA 系列）保持一致的归档方式。

## 实现方式

### 改动文件

| 文件 | 改动 |
|---|---|
| `server/include/looper.hpp` | 新增 `kMaxDispatchRounds = 64` 与 `dispatchUntilDrained()` 声明 |
| `server/src/looper.cpp` | 实现 `dispatchUntilDrained()`；主循环改为"每轮都派发到清空"，并在 poll 超时分支检查 `dispatch_status` 主动排空 |
| `server/test/unit/test_looper_dispatch_gtest.cpp` | 新增回归测试（4 个用例），自建私有 `dbus-daemon` |
| `server/test/CMakeLists.txt` | 注册 `test_looper_dispatch_gtest` 到构建与 CTest |
| `tools/interference_test.py` | 测量方式由 `dbus-send` 子进程改为复用单条连接 |
| `docs/架构设计-模块与代码结构.md` | 新增"派发层三条硬约束"小节 |
| `docs/学习路线图.md` | Looper 学习要点补充派发语义 |

### 关键决策

1. **派发函数提升为头文件声明的生产函数**（原为文件内 `static`）。
   理由：原会话写的测试只是**镜像复刻**了循环逻辑，等于在测 libdbus 而非
   测我们的代码，回归保护价值接近于零。提升后有真实生产调用方
   （`Looper::run`），测试能直接验证生产实现，且不构成孤儿接口。

2. **上限 64 轮**。不能写成无界 `while`——`read_write_dispatch` 的返回值语义
   是"连接未断开"而不是"还有数据"，持续有内部消息时无界循环会永久饿死
   self-pipe 的停止信号，导致 SIGTERM/SIGINT 失效、主线程空转。
   64 轮在正常负载下远超实际积压量，且足够小以保证停止信号及时被处理。

3. **测试自建 `dbus-daemon`**。原会话的测试用 `DBUS_BUS_SESSION`，
   而 CI（GitHub Actions）与开发板（systemd 服务）都没有会话总线，
   测试会整体 SKIP——一条永远被跳过的回归测试等于没有回归测试。
   现改为 `fork + execlp("dbus-daemon", "--session", "--nofork", "--print-address=1")`，
   用完即杀；连 dbus-daemon 都不可用时显式 SKIP 并打印原因。

### 验证手段

**RED→GREEN 闭环（用真实变异验证测试有效性）**：

把 `dispatchUntilDrained()` 临时改回"只派发一次"，重编译后运行测试：

```text
[  FAILED  ] LooperDispatchTest.DispatchUntilCompleteDrainsWholeQueue
[  FAILED  ] LooperDispatchTest.DispatchRoundLimitIsBoundedAndFinite
[  PASSED  ] 2 tests.
```

恢复修复后连续 3 次运行均 4/4 通过。第一版上限断言只检查"还有积压"，
在旧代码上会**假通过**（单次派发同样留积压），已改为按**处理条数**断言
（恰好等于 64），使"只派发一次"与"无界循环"两种错误都能被捕获。

**x86 全量单测**：`ctest --test-dir build-x86/server` → **54/54 passed**。

**ARM64 交叉编译**：`./tools/ci.sh --local-only` → 编译 + 打包成功。

**板端部署与真机测量**（Radxa Cubie A7A，`./tools/ci.sh --skip-push`）：

| 指标 | 修复前 | 修复后（空载） | 修复后（干扰负载） |
|---|---|---|---|
| 服务端处理 P99（复用连接） | 21ms 级 | **1.33ms** | **4.04–6.14ms** |
| 五个方法 max（子进程方式） | 3027ms（最坏 25s） | 18.6–25.4ms | — |
| 失败数 | — | **0 / 200** | 0 |

干扰测试（`tools/interference_test.py 60`）三项预算：

```text
D-Bus HealthCheck P99   : 6.14 ms (预算门限: < 20.0 ms) -> ✅ 合格
weaknet-server RSS 波动 : 0.90 MB (预算门限: < 5.0 MB)   -> ✅ 合格
SoC 最高核心温度        : 52.4 ℃ (预算门限: < 75.0 ℃)  -> ✅ 合格
```

**systemd 停服未饿死**（验证 64 轮上限没有破坏信号响应）：
`systemctl stop weaknet-server` 耗时 **2067ms**，状态 `inactive`，
未触发 `TimeoutStopSec` 后的 SIGKILL；重启后服务 `active`，
`HealthCheck` p99 1.56ms、零失败。

**板端冒烟**：`weaknet-test-full.sh` → **PASS 26 / FAIL 0 / SKIP 1**。

## 结果

### 关键收获：测量陷阱

修复后仍有一步差点误判。用 `dbus-send` 子进程逐次调用时，
P99 长期停在 18–26ms，一直"超标"。这不是服务端问题——
**每次 `dbus-send` 的 fork+exec+总线认证固定要 11~13ms**，
它在分母里占了绝大部分。改用复用连接后同一服务的 P99 是 1.3ms。

`tools/interference_test.py` 已改为默认复用单条连接，
并保留 `--legacy-cli` 作为对照。这一条已写进
`docs/架构设计-模块与代码结构.md` 的测量陷阱提示，
避免下次又用错方法得出错误结论。

### 遗留

1. **`./tools/ci.sh --skip-push` 的 adopt 步骤报错**（`phase=COMMITTED 不允许 commit`，
   rc=3）。这是**重复部署同一版本**时的幂等分支未覆盖：健康窗口已通过、
   版本实际已 `COMMITTED`、新二进制已生效（时间戳与大小与本地一致）、
   服务正常运行，因此**不影响本次交付**；但脚本在"目标版本 == 当前已提交版本"
   时应当走 no-op 而不是报错退出。这是独立缺陷，本次未修。
2. **派生度量仍在用子进程方式**：`tools/interference_test.py --legacy-cli`
   以及其他历史脚本里的 `dbus-send` 延迟测量同样含进程启动开销，
   如需用于 SLO 判定需逐一改造。
3. **未验证长时间高负载下的队列增长**：本次只测了 60 秒干扰负载，
   64 轮上限在极端积压（如 1000+ 条待派发）下的表现未做压力测试。

### 后续方向

- 修 `tools/ci.sh` adopt 的"同版本重部署"幂等分支（独立小修）。
- 若将来出现方法调用抖动，先看 `dispatch_status` 是否长期为
  `DATA_REMAINS`，再怀疑处理逻辑——派发层已不再是可疑点。
