# 看门狗与 OTA 完整闭环：墙钟跳变豁免 + 制品链修复 + apply 致命缺陷

> 日期：2026-11-21
> 分类：工作流外实施（bug 修复 + 文档）
> 结果：完成

## 构思

用户问「看门狗和 OTA 这两个功能是否完整闭环」，答案是不能。逐项核对后确认三处断点：

1. **看门狗被墙钟跳变误伤（尚未暴露但必然复发）**
   本板没有 RTC 电池，`systemd-timesyncd` 开机时先恢复记录时间戳，再由 NTP 一次性
   大跳变校正。2026-11-21 05:49:11 实测跳了 **9785 秒**。守卫所有「文件 mtime 距现在
   多久」的判据都会算错：`wal_mtime` 看到 `age_s=9785` 判 FAIL。当时只靠 `kDebounceK=2`
   去抖捡回一条命 —— 但这只是运气，同一次跳变还会：
   - 让维护标记提前过期，在 `systemctl restart` 中途**悄悄撤掉保护**；
   - 把正在进行的 OTA 回执判成 `STALLED`，服务正停着就恢复判定 → 升级中途复位。

2. **新板子拿不到看门狗（`ci.sh` 只落盘不 enable）**
   此前刻意不自动 enable 是怕判据未成熟就复位整机，但结果是「新板永远没有看门狗」，
   保护形同虚设，且没有任何提示。

3. **OTA 制品链与现场不一致，且 `apply` 路径存在致命缺陷**
   - `artifacts/weaknet-1.1.0-arm64.tar.zst` 停在 2026-10-21，36 个文件里
     **0 个** health-guard；板端 `releases/1.1.0` 有 33 个文件、含 health-guard。
     `release.sh:131` 在 11-16 已补上该文件，但从没重新执行过发布。
   - `release.sh` 用 `cp -rL` 打包，把 SONAME 软链**解引用**成 3 份完整副本
     （包 5 MB → 12 MB），制品布局与板端 release 目录不一致。
   - 最严重：`weaknet-local-update.sh:444` 的 `local rel="$1" src="$rel/..."`。
     bash 的 `local` 在同一行内的展开发生在变量创建**之前**，`$rel` 取外层作用域。
     `cmd_adopt` 外层恰好有 `local rel=`（动态作用域），所以 `ci.sh` 部署路径一直
     侥幸正确；而 `cmd_apply` —— **云端下发 OTA 的唯一路径** —— 外层没有 `rel`，
     `set -u` 直接致命退出。

## 实现方式

### 一、看门狗：单调钟 vs 墙钟差值检测

新增 `ClockStepDetector` / `DetectClockStep()`：每轮比较 `CLOCK_REALTIME` 与
`CLOCK_MONOTONIC` 的增量差，超过 `kClockStepThresholdSec=5` 即判定本轮机钟跳变。

四个受影响的判据统一走 `ClockStepExempt()`，一律**按「新鲜」方向豁免一轮**：
`wal_mtime`（年龄不可信）、维护标记（当作仍有效）、OTA 回执（当作仍在进行）、
trip 历史清理（不清空真实历史）。方向选择是 fail-safe：宁可少判一次，
不可误撤保护。

豁免上限 `kClockStepMaxStreak=3`（6 秒）：NTP 校正是一次性的，跳完下一轮就恢复；
超过 3 轮说明时钟本身不可信，这时必须让判据的失败被看见，而不是永久装作健康。

`ci.sh` 新增 `--enable-guard`：先 `--dry-run` 观察 20 秒（必须产出至少一行
`wal_mtime=` 才认，因为前 12 秒是 WARMUP，无 FAIL 不代表健康），判据全绿才
`systemctl enable`（不 `--now`，避免抢占 single-open 的 `/dev/watchdog0`）。

### 二、OTA 制品链

- `release.sh`：`cp -rL` → `cp -a`，保留 SONAME 软链。
- 重新执行 `release.sh --skip-build`，制品从 36 个条目 / 12 MB 收敛到
  36 条目 / 10 MB，软链形态与板端 release 目录逐项一致。
- `weaknet-local-update.sh:444` 拆成两行 `local`，并附完整故障链注释。

### 三、验证

- 板端真实跳变实验：`CLOCK_STEP wal_mtime step=500s streak=1/3 -> exempt this round`，
  随后 `wal_mtime=ok(clock_step=500s, age unknown (exempt),f0)`，下一轮恢复正常。
- 沙盒用**真实签名制品**跑 `apply`：修复前 `rel: unbound variable` 致命退出
  （bash 5.1.4 与 5.2.15 各复现一次，非解释器差异）；修复后完整走完
  `PREPARED → SWITCH_PENDING → TRIAL → COMMITTED`，指针翻转 + `.committed` 落地。
- **端到端**：用**包内自己那份** `weaknet-local-update.sh` 跑 apply，同样跑到 COMMITTED。
- 回归：P0-A 44 断言 / P0-B 30 断言 / P0-C1 36 断言全通过；OTA 单测 91+55+32=178 项
  全通过；看门狗单测 126 项通过；x86 全量 ctest 71/71 通过。
- 板端：修复后的脚本已落盘到 `$ROOT/` 与 `current/`，manifest 30 个文件全 OK。

### 四、文档

- `docs/OTA固件更新与安全发布指南.md`：
  新增 §1.0「制品边界：包内放什么、不放什么」，明确 systemd 单元与 OTA 控制器
  **不随包发布**及其三条硬理由，并承认「只走远程 OTA 的设备拿不到新单元」这条边界；
  §4.2 新增「发布后必做的一步：用真实包跑一次 apply」，附实测教训与验收脚本。
- `CLAUDE.md`：新增墙钟跳变豁免说明与板端验证手法（含「timesyncd 会秒改回来，
  看不到 CLOCK_STEP 不代表无缺陷」这个陷阱）。

## 结果

三处断点全部闭环。但有两处必须明确边界：

- **P0-C3 断电自愈仍未完成**：只测过 `trial` 一个时机，`arm-switch` / `do-switch` /
  `commit-immediate` 未测，无 `write through` 对照，也无重复统计。板端 `write_cache`
  是 `write back`，单次成功不能排除概率性丢失。**不得宣称断电自愈完成。**
- **单元/控制器不随 OTA 包发布是设计选择，不是待修缺陷**。远程 OTA 能更新业务
  二进制，不能更新保护业务的那一层。已写进指南而不是留着不说。

### 未做的事

- 没有创建 OpenSpec change（按 `no-new-openspec-change-by-default` 记忆，走工作流外）。
- 没有提交 git（未获用户授权）。
- 没有重跑 ARM64 全量编译 —— 制品复用的是现成 `dist-arm64`，已逐项 sha256 比对确认与
  板端 `current` 一致。
