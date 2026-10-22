# OTA P0-B 开机恢复服务与最小 SSH 救援

> 日期：2026-10-22
> 分类：工作流外实施（承接 [[2026-10-22-ota-p0a-transaction]]）
> 结果：已完成（恢复器 + 单元测试 + 沙盒演练 + 真机部署与演练）

## 构思

### 要解决什么

P0-A 交付了事务状态机与崩溃点**判定**（`reconcile --dry-run`），但判定
本身不做事。用户明确指出：

> **"升级机制的可靠性不能完全依赖被升级的 weaknet-server 本身。"**

以及：

> **"在独立恢复机制完成并通过故障注入测试之前，不应宣称断电自愈已经完成。"**

P0-B 补上"做事"的那一半：一个**独立于 weaknet-server** 的开机恢复服务。

用户给的三个验证重点：

1. 即使升级脚本消失也能读取事务
2. 即使 weaknet-server 无法启动也能完成回滚决策
3. 即使回滚失败也能保留 SSH 救援入口

### 关键判断

**恢复能力不能绑在会被升级掉的东西身上。** 升级脚本 `weaknet-local-update.sh`
住在 `$ROOT` 下，会随 OTA 制品被覆盖/删除；`weaknet-server` 正是被升级的
对象。所以恢复器必须：

- 只读 `/var/lib/weaknet-ota/transaction.json`（不随版本切换）
- 装在 `/usr/local/lib/weaknet/`（不随 release 走）
- systemd 单元里**不写** `Requires=weaknet-server.service` / 强 `After=`
- **从不引用** `weaknet-local-update.sh`（源码级门禁，单测 + 演练双重校验）

### 为什么不做进 OpenSpec change

与 P0-A 同理：交付面是**板端升级执行体与运维接口**，不新增对外协议、不改
云端契约、不新增服务端 surface。恢复服务的消费方是运维人员与云端未来的
订阅通道（尚不存在）。

按 `docs/ai/implementation-economy.md` 的判据，没有产生需要独立评审的产品
surface，因此走工作流外实施并在此留记录。

### 备选方案与取舍

| 方案 | 取舍 |
|------|------|
| A. 恢复逻辑写进 `weaknet-local-update.sh` | ❌ 该脚本会被升级覆盖；升级包解包失败时它本身可能已损坏 —— 恢复能力必须独立于它 |
| B. 恢复逻辑写进 C++ 服务端 | ❌ 服务端正是被升级对象；服务不在线就没有恢复能力 |
| C. 独立 Python 恢复器 + 独立 systemd 单元（选中） | ✅ 单文件纯标准库；板端 Python 3.9 实测可跑；单元不依赖 weaknet-server 就能起来 |
| D. 纯 shell 脚本 | △ 也可行，但判定逻辑（13+ 分支）用 shell 表达难以保证正确性与可测性 |

## 实现方式

### 改动文件

| 文件 | 性质 | 内容 |
|------|------|------|
| `tools/weaknet-ota-recover` | 新增 | 恢复器（~800 行 Python 3，只用标准库）：纯函数决策 `decide()`、健康探测 `Prober`、动作编排 `Watcher`、救援模式 `Recoverer.enter_rescue()` |
| `tools/test_weaknet_ota_recover.py` | 新增 | 35 项单元测试：只读事务、健康判据、救援模式、动作编排、控制器缺失兜底 |
| `tools/ota_p0b_drill.sh` | 新增 | 21 项断言的沙盒演练（8 节）：无事务、健康转正、尝试耗尽回滚、崩溃循环、控制器缺失兜底、状态损坏救援、救援报告、升级脚本消失仍可恢复 |
| `tools/weaknet-ota-recover.service` | 新增 | systemd 单元：开机自启，`SuccessExitStatus=75`（窗口耗尽请求重启计下一次尝试） |
| `tools/weaknet-ota-ctl` | 修改 | `bump-boot-attempt` 按 `boot_id` 幂等；`recover --accept` 保留 `previous_stable_version`；`bootstrap` 分层推断并记录 `source` |
| `tools/ci.sh` | 修改 | 部署恢复器与单元到板端；缺失只告警（它影响"开机自愈"而非"能不能升级"） |
| `.github/workflows/ci.yml` | 修改 | 增加 P0-B 门禁（单测 + 演练） |
| `docs/OTA固件更新与安全发布指南.md` | 修改 | 新增 §1.4（P0-B 设计与边界）、§5.1（两套自测）、§5.3（手工排障） |
| `CLAUDE.md` / `docs/架构设计-模块与代码结构.md` | 修改 | 意图表补 3 条；目录树补恢复器与单元 |

### 关键决策

1. **决策是纯函数**：`decide(state, current, healthy, elapsed, restarts, attempts)`
   不碰文件系统、不碰服务。全部外部命令走可注入的 `runner`，所以单测能
   覆盖完整行为链而不需要真机。
2. **健康判据：`systemd is-active` 是硬门禁，D-Bus 不可达不算健康但允许
   继续重试**。绝不把超时降级成成功——但也不立刻判死。
3. **崩溃循环用 `systemctl show NRestarts` 抓**：试运行版本一启动就崩，
   重启次数增长本身就是"这版本不行"的硬证据，不必等满窗口。
4. **尝试次数持久化**：`boot_attempts` 存在事务状态里，**重启后不归零**，
   这正是拦住"断电无限重试"的关键。计数按内核 `boot_id` 幂等——恢复服务
   自身被 systemd 重启不算一次新尝试。
5. **救援模式 = 最小可达**：停服 + mask（不可用时退到 disable）+ 保证
   `ssh` / `weaknet-tunnel` active + 写 `RESCUE.md`。**不做版本切换**
   （未知状态不能继续传播）、**不做无限重启**、**不启动 weaknet-server**。
6. **控制器缺失时退回直接原子翻链**（symlink + rename + fsync），并显式
   告警。救援路径不能因为一个文件缺失就彻底失效——但这条路径不受事务
   保护，所以只在控制器确实不可用时使用。

### 真机演练暴露的三个真缺陷

| 缺陷 | 根因 | 修法 |
|------|------|------|
| 救援模式 mask 失败 | 板端 unit 文件在 `/etc/systemd/system/` 下，`systemctl mask` 报 "File already exists" 退出码 1 | 加 `disable` 兜底路径（同样达到"systemd 不拉起"的效果），并如实报告实际用了哪种手段 |
| `recover --accept` 丢掉 `previous_stable_version` | 事务里 `previous_stable_before` 为空时直接写 `None` | 回落到状态里已有的值；且 `prev == accepted` 时清空（否则没有退路）+ 两条回归测试 |
| 恢复服务被自身重启时重复计数 | `bump-boot-attempt` 无幂等 | 按内核 `boot_id` 幂等：设备真重启才换 id |

### 验证手段

**本地（x86 宿主机）**

```
python3 tools/test_weaknet_ota_ctl.py       → Ran 74 tests, OK
bash tools/ota_p0a_drill.sh                 → PASS=44 FAIL=0
python3 tools/test_weaknet_ota_recover.py   → Ran 35 tests, OK
bash tools/ota_p0b_drill.sh                 → PASS=21 FAIL=0
```

**板端（Radxa Cubie A7A，aarch64，Python 3.9.2，systemd 247）**

- 35 项单元测试 + 21 项演练断言在板端 Python 3.9 上**全绿**
- 恢复器实装到 `/usr/local/lib/weaknet/weaknet-ota-recover`（root:root 0755）
- `weaknet-ota-recover.service` 已 `enable` + `active`，开机自启
- 单元**不写** `Requires=weaknet-server.service`——恢复服务不依赖被恢复对象

**真机行为演练（注入故障 → 观察恢复 → 人工恢复）**

1. **健康转正**：注入 `TRIAL`（to=1.1.0，current 已指向 1.1.0）→ 恢复服务
   探测到全绿 → `boot_attempts` 加一 → 经控制器 `commit` → `phase=COMMITTED`。
   日志：`判定：action=commit（试运行版本健康，转正）healthy=True detail=全绿`。
2. **故障 → 救援**：注入 `TRIAL`（to=1.2.0，但 current 仍指向 1.1.0）→
   判定 `rescue`（指针被外力改动）→ 停服 + disable + 保 ssh/tunnel +
   写 `RESCUE.md`（含现场信息表与人工恢复步骤）。
3. **人工恢复**：`require-recovery` → `recover --accept 1.1.0` → 事务转
   `COMMITTED` → 服务 restart 后 `active`，`readlink current` =
   `releases/1.1.0`，`weaknet-cli update status` =
   `installed_version=1.1.0, current_committed=true`（基线不变）。
4. 演练残留已清理（`RESCUE.md` 删除，临时目录删除，`previous_stable_version`
   还原为 1.0.0）。

**真机临时文件已清理**（`/tmp/p0b`、`/tmp/wn-p0b-*` 全部删除）。

## 结果

### 已完成

- 恢复器、systemd 单元、单元测试、沙盒演练、部署链路、文档同步全部落地
- 真机部署 + 真机行为演练通过（健康转正 / 故障救援 / 人工恢复三段闭环）
- 三条硬约束均有可执行断言守护（源码门禁 + 沙盒演练 + 单元测试）

### 遗留（**明确未做，不得宣称完成**）

1. **真正的掉电自愈未验证** —— `fsync` 保证的是「内核 → 设备」，不保证设备
   写缓存已落到介质；板端根分区是 `/dev/sda3`（SATA/USB），若该盘启用写
   缓存且未走 FUA，掉电仍可能丢失已 fsync 的数据。当前全部验证都用状态
   构造与 `kill` 代替，**不等于掉电验证**。这是"断电自愈"这一说法的硬边界。
2. **真机未做真实的升级中断电注入** —— 板端演练是"注入事务状态"，不是
   "升级跑到一半真的断电"。前者验证了恢复逻辑，后者还依赖上面的 fsync
   语义与介质行为。
3. **云端侧尚无恢复事件上报通道** —— 恢复服务在板端本地完成恢复，云端拿不到
   "这台设备发生过恢复"这件事。需要云端订阅通道（后续议题）。
4. **救援模式未在真机上验证"从救援模式恢复后服务仍能正常工作"** —— 已验证
   服务 `active` 与版本基线，但没有跑完整的功能回归（`weaknet-test-full.sh`）。
5. **密钥轮转建议仍然有效**（见 [[2026-10-22-ota-release-pipeline-closure]]）。

### 后续方向

- **P0-C（设想）**：云端订阅恢复事件 + 掉电测试台（可控断电的验证装置）
- 恢复策略可配置（当前 `MAX_TRIAL_ROUNDS=3` / `MAX_TRIAL_RESTARTS=3` /
  `HEALTH_TIMEOUT_S=120` 是硬编码常量，未进配置白名单）
