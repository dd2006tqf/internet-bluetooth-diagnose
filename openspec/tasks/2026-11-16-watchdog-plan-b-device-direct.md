# 看门狗路线收尾：删除自建模块，直接喂平台驱动导出的 /dev/watchdog0

> 日期：2026-11-16
> 类型：**工作流外实施**（无活跃 OpenSpec change，`active_change: null`）
> 关联：`openspec/tasks/2026-11-14-system-health-watchdog-plan-b.md`（决策与实测记录）
> 影响面：`kmod/`、`tools/`、`client/CMakeLists.txt`、简历知识库、`docs/ideas/`、`CLAUDE.md`

## 一、构思

### 要解决的问题

`weaknet-health-guard` 的职责是判断「采集服务还健康吗」，不健康就停止喂狗让硬件复位。
到 2026-11-15 为止，它靠一套自建内核模块实现：`weaknet_wdt.ko` 自注册一个 watchdog
设备并抢过 `0x02050000` 那块寄存器，再由 `weaknet-wdt-takeover.sh` 按
`insmod → 确认节点 → unbind sunxi_wdt` 的顺序把原厂驱动赶走。

这套东西能跑，但代价很重：

- 327 行内核模块 + 313 行接管脚本，每次部署多两条容易写错的路径；
- `insmod` 与 `unbind` 之间必然存在「旧狗已撤、新狗未起」的真空窗口；
- 归还逻辑必须靠 `ExecStopPost` 兜，而它有一整类「静默取消已排定复位」的缺陷
  （`rmmod` 经 `watchdog_stop_on_unregister` 调 `.stop`，bind 回原驱动让模块退场，
  两者都会关掉硬件看门狗）。

### 关键发现：接管是多余的

读 `drivers/watchdog/watchdog_dev.c` 后确认 `watchdog_need_worker()`：

```c
return (hm && watchdog_active(wdd) && t > hm) ||
       (t && !watchdog_active(wdd) && watchdog_hw_running(wdd));
```

`sunxi_wdt` 从不设 `max_hw_heartbeat_ms` ⇒ `hm = 0` ⇒ 第一项恒假；
用户态 `open()` 之后 `WDOG_ACTIVE` 置位 ⇒ `!active` 为假 ⇒ 第二项恒假。

**两项同时为假 = 内核彻底停止代喂。** 也就是说，「接管喂狗权」这个动作的完整实现
就是 `open("/dev/watchdog0")`，不需要 unbind，更不需要自建设备。

配合两个已验证的板端事实：

- `WDIOC_GETSUPPORT` 返回 `options = 0x8180`，含 `WDIOF_MAGICCLOSE`
  ⇒ 停止语义（不写 `'V'` 就停不下来）与原厂驱动完全一致；
- `WDIOC_GETTIMEOUT` 返回 16，可调范围 `1..16`。

两条路线语义等价，方案 B（直接喂平台设备）少一整套复杂度。

### 为什么不是「顺手改一下」

删掉模块会同时打断四类依赖，任何一处漏改都会在编译或部署阶段才炸：

| 依赖 | 漏改后果 |
| :--- | :--- |
| `kmod/Makefile` 的 `obj-m += weaknet_wdt.o` | 交叉编译失败（源文件不存在） |
| `tools/ci.sh` 的 `install ... weaknet-wdt-takeover.sh` | `set -e` 下整段打包中止 |
| `tools/test_weaknet_health_guard.py` 三个测试类 | 打开已删脚本 → `FileNotFoundError` |
| `tools/weaknet-health-guard.service` 三个 hook | 单元起不来或归还逻辑跑空 |

所以按「先让整棵树自洽，再验证」的顺序做，而不是先删文件。

## 二、实现方式

### 1. `tools/weaknet-health-guard.cpp`（唯一实质改动）

| 改动 | 内容 |
| :--- | :--- |
| 头注释 | 重写为方案 B 路线说明：标准设备、内核停止代喂的机制、三条退出路径、`MAGICCLOSE` 是唯一外部依赖 |
| 设备常量 | `kWatchdogDev = "/dev/watchdog"`、`kWatchdogAlt = "/dev/watchdog0"`（替换 `/dev/weaknet_wdt` + `/dev/watchdog1`） |
| 新增 `VerifyDeviceContract()` | 启动期硬断言 `WDIOF_MAGICCLOSE` 与 `WDIOC_GETTIMEOUT >= kTickSeconds * kDebounceK * 2` |
| 重写 `OpenWatchdog()` | 先 misc 节点后动态编号；两个都失败时优先回报 `EBUSY`（「被人占着」与「不存在」处置不同） |
| `main()` 插入契约检查 | 在首次喂狗之前；失败时**写 `'V'` 再 close**，而不是直接 `return` |
| 新增 include | `<sys/ioctl.h>`、`<linux/watchdog.h>` |

`VerifyDeviceContract()` 里两个刻意的决定：

- **不读 `WDIOC_GETBOOTSTATUS`**：`sunxi_wdt` 没实现 `.bootstatus`，该值恒为 0，
  读它只会制造「我有依据」的错觉。
- **契约失败必须先写 `'V'`**：`open()` 已经把硬件使能了。直接 `return` 会让 fd 被内核
  关掉，走的是「没写 `'V'` → 拒绝停狗 → 硬件超时复位」那条故障路径 ——
  一个配置错误会变成无限复位。

全程只有 `write(fd, "1", 1)` 与 `write(fd, "V", 1)`，**零 ioctl 改硬件状态**。
`WDIOC_SETTIMEOUT` / `SETOPTIONS` / `SETPRETIMEOUT` / `KEEPALIVE` 一个都没有：
一旦用 ioctl 去 arm/disarm，停止喂狗就不再是唯一出口，不变量被绕过。

### 2. `tools/weaknet-health-guard.service`

删掉三个钩子：

```
ExecStartPre=.../weaknet-wdt-takeover.sh take
ExecStartPost=... 等 /dev/watchdog1
ExecStopPost=.../weaknet-wdt-takeover.sh give-back
```

换成一条等待平台设备的 `ExecStartPre`（最多 3 秒），并在单元里写清
**为什么刻意没有 `ExecStopPost`**：驱动从未被 unbind，没有需要归还的东西；
停止语义完全由设备的 `MAGICCLOSE` 契约在内核 `release()` 里执行。

`Restart=no` + `RestartPreventExitStatus=99` 保持不变，注释里补上「退出码 99 是
纵深防御：万一将来有人把 `Restart` 改成 `on-failure`，它依然能挡住重新 open 喂狗」。

### 3. 删除

| 文件 | 行数 |
| :--- | ---: |
| `kmod/weaknet_wdt.c` | 327 |
| `tools/weaknet-wdt-takeover.sh` | 313 |

### 4. 清残留

- `kmod/Makefile`：去掉 `obj-m += weaknet_wdt.o` 与其注释块，改为一段「为什么移除」
  的说明 + 历史实现指引（`git show beff747:kmod/weaknet_wdt.c`）。
- `tools/ci.sh`：删除 `install ... weaknet-wdt-takeover.sh`（否则 `set -e` 下打包中止）。
- `tools/kmod_cross_build.sh`：日志文案从「观测模块 + 看门狗模块」改为「观测模块」。
- `client/CMakeLists.txt`：注释里 `/dev/weaknet_wdt` → `/dev/watchdog`。

### 5. `tools/test_weaknet_health_guard.py`

删除 3 个引用已删脚本的测试类（共 6+1+N 项，全部会 `FileNotFoundError`）：

- `TestTakeoverScriptOrdering`
- `TestGiveBackDoesNotReturnOnUncleanExit`
- `TestGuardExitSemantics`

改写 `TestSystemdUnitConsistency`：`test_has_stop_post_for_give_back` →
`test_has_no_stop_post_hook`（断言**不应**有 `ExecStopPost`，理由写进 docstring），
新增 `test_pre_waits_for_platform_device`。

新增两个类：

- **`TestDeviceContract`（7 项）** —— 只读源码、不需要板子，每条对应一个
  「保护看起来在、实际不在」的失效模式：
  打开平台设备而非私有节点、`MAGICCLOSE` 硬断言、超时下限、
  契约失败先写 `'V'` 再 close、禁止改硬件状态的 ioctl、刻意不读 bootstatus、
  契约通过后立刻首次喂狗。
- **`TestGuardDoesNotRepairService`（2 项）** —— 钉死「守卫不拉起服务」：
  代码里没有 `systemctl start/restart/stop/enable`，唯一的 systemctl 用法是只读的 `show`。

> **测试写法上的一个坑**：这两组断言是「源码中不得出现某符号」，而源码注释恰好在
> 解释「为什么不能那么做」，会把符号名写进去。第一版因此失败。修法不是删注释，
> 而是给测试加 `_strip_c_comments()`，只断言**代码**不断言注释。
> 不剥注释会诱导后来者删掉有价值的说明。

## 三、结果

### 验证（全部本地，开发板当时不在线）

| 项 | 命令 | 结果 |
| :--- | :--- | :--- |
| 判据测试 | `python3 tools/test_weaknet_health_guard.py` | **89 项通过** |
| x86 编译 | `cmake --build build-x86 --target weaknet_health_guard` | 通过，ELF x86-64 |
| ARM64 交叉编译 | `cmake --build build-arm64-native --target weaknet_health_guard` | 通过，ELF aarch64 |
| shell 语法 | `bash -n tools/ci.sh`、`bash -n tools/kmod_cross_build.sh` | 通过 |
| 残留扫描 | `grep -rn 'weaknet-wdt-takeover\|kmod/weaknet_wdt'` | 仅剩历史留痕（见下） |

### 文档同步

| 文件 | 改动 |
| :--- | :--- |
| `弱网项目简历知识库.md` | 条目六**重写**：简历写法、断言表、开发过程 1~7、背景知识 A~E、30 秒模板；检索导航的事实速查表与索引行同步；「不可主张」表把两条旧限制换成两个新坑 |
| `CLAUDE.md` | 已改为方案 B 表述（直接喂 `/dev/watchdog0`、`MAGICCLOSE` 契约） |
| `docs/ideas/喂狗条件检查-系统健康看门狗.md` | 顶部加「本文档有前后两版方案」声明 + 逐段标注作废；不做历史改写 |
| `openspec/tasks/2026-11-14-...-plan-b.md` | 补 §6.4（停喂复位实测）与 §6.5（路线修正删除清单） |
| `openspec/pitfalls/install-no-parent-dir-with-set-e.md` | 加时效性说明（举例的文件已删，教训仍成立） |

### 简历条目的最终写法

> 基于 Linux 标准看门狗子系统实现硬件看门狗守护进程，以数据落库、采集推进、
> 进程活性、阻塞状态四项判据判定采集服务健康，持续失败即停止喂狗触发整机复位
> 并留存故障现场快照。

相对初版的改动：删掉「内核内存取证定位喂狗寄存器」「自注册 watchdog 设备」
「有序接管原驱动」（这三条对应的代码已删除），补上「判定采集服务健康」
（这是这条功能的真正目的，旧版没写出来）。

### 遗留边界（不得越界声明）

- **真机端到端已验证**：停喂 → `boot_id` 变更 → 约 28 秒。**不是 16 秒**，
  16 秒只是硬件计时下限。
- **默认不启用**：`ci.sh` 只落盘单元、不 `enable`。且因为不 enable，
  一次复位之后保护就没了，需要人工再拉起。
- **没有跨 boot 触发计数**：`RestartTracker` 在内存里，复位后归零，
  确定性故障会无限复位且无升级路径。已知缺陷，未修。
- **刻意不拉起采集服务**：修复期继续喂狗等于关掉保护。守卫只负责「发现」。
- **部分采集停止不被捕获**：禁用 5/7 个活跃监控器后比例仍是 2/2 = 100%（
  被禁用的时间戳归 0，等于从分母剔除）。能捕获集体停更，不能捕获部分停止。
- **失败 JSON 不保证留存**：`/dev/sda3` 是 `write back`，`fsync` 只保证「内核 → 设备」。

### 未做

- 「采集停但进程活」的验收（已在 `--dry-run` 中观察过判据行为，但未做端到端复位验证）。
- 1 小时接喂狗空跑（原 6.3 未走完，路线已改）。
- `D` 状态长稳基线。
- 开机时看门狗是否已被使能的只读取证（`0x02050000 + 0x14` enable 位）。
