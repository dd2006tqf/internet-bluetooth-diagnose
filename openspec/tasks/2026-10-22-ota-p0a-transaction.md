# OTA P0-A 升级事务状态机

> 日期：2026-10-22
> 分类：工作流外实施（承接 [[2026-10-22-ota-release-pipeline-closure]]、[[2026-10-21-ota-cloud-dispatch]]）
> 结果：已完成（控制器 + 测试 + 演练 + 真机部署验证）

## 构思

### 要解决什么

用户提出的判断：**"升级机制的可靠性不能完全依赖被升级的 weaknet-server 本身。"**

回看 P0-A 之前的 `weaknet-local-update.sh`，可靠性有一个结构性缺口：

`current` 指针、`.previous` 回滚指针、`.committed` 转正标记，由 `cmd_apply` /
`cmd_rollback` / `cmd_adopt` / legacy 迁移 / `--commit` **五条执行路径各自
读写**。升级脚本一旦在关键窗口被掉电或杀死，**没有任何一方知道这次升级做到
哪一步**：

- 指针翻了但还没转正 → 设备跑在未验证版本上，没有任何记录说"这次升级没完成"
- 转正了但 `.committed` 没写 → C++ `update_status.cpp` 报 `current_committed=false`
- `.previous` 被写成了失败的候选版本 → 之后的无参回滚会回滚到那个失败版本
- 同版本重装时 `current` 指向的目录被"先挪走再挪进" → 掉电即砖

也就是说：加一份事务日志，只能让设备在重启后**知道发生了什么**，还不能
**自动完成恢复**。所以本次交付明确定位为 P0-A（可观察 / 可判定 / 可显式修复），
P0-B（开机自动恢复）单独立项。

### 三个保证的拆解

用户给的三条保证：

| 保证 | P0-A 交付了什么 | P0-A 交付不了什么 |
|------|-----------------|------------------|
| (a) 升级失败不能让设备持久不可达 | 指针与状态分离；失败候选绝不进 previous/rollback_target；同版本重装用原子交换 | 断电后无人自动翻回指针 |
| (b) 弱网/断网/重启后能恢复升级流程 | 13 个崩溃点每个都有唯一且非破坏性的判定；`reconcile` 只读可查 | 开机自动执行恢复动作 |
| (c) 云端知道每台设备的版本/阶段/结果 | 事务状态含 `from → to`、`kind`、`phase`、`boot_attempts`、`last_error`，可查可回传 | 云端侧尚无订阅通道 |

### 为什么不做进 OpenSpec change

本次交付的产品面是**板端升级执行体与运维接口**，不新增对外协议、不改云端
契约、不新增服务端 surface。事务状态文件与控制器命令是**升级执行体内部的
实现细节**，消费方是运维脚本与 P0-B 恢复服务（后者尚未存在）。

按 `docs/ai/implementation-economy.md` 的判据，没有产生需要独立评审的
产品 surface，因此走工作流外实施并在此留记录。

### 备选方案与取舍

| 方案 | 取舍 |
|------|------|
| A. 事务逻辑写进 `weaknet-local-update.sh` | ❌ 该脚本会随每次升级自我覆盖（`install_self_script`），1.1.0 与 1.2.0 的事务语义可能不同，而恢复服务读的是同一个 `transaction.json` —— schema 必须由**不随版本变化**的组件写 |
| B. 独立 Python 控制器（选中） | ✅ 装在 `$ROOT` 之外（`/usr/local/lib/weaknet/`），单文件无依赖（只用标准库），板端 Python 3.9 实测可跑 |
| C. 事务状态放在 `$ROOT/releases/` 里 | ❌ 会被 GC / release 切换波及，且与"不随版本变化"要求冲突 |
| D. 控制器写进 C++ 服务端 | ❌ 服务端正是被升级的对象，且服务不在线时就没有升级能力 —— 与"可靠性不能依赖被升级对象"直接冲突 |

## 实现方式

### 改动文件

| 文件 | 性质 | 内容 |
|------|------|------|
| `tools/weaknet-ota-ctl` | 新增 | 事务控制器（~1300 行 Python 3，只用标准库）：10 个阶段、原子落盘、flock 单写者、`reconcile()` 崩溃点判定、保守 GC、兼容投影、16 个 CLI 子命令 |
| `tools/test_weaknet_ota_ctl.py` | 新增 | 74 项单元测试（schema / 状态机 / 崩溃矩阵 / GC 保守性 / 投影 / bootstrap 推断来源 / materialize / 首次安装 / CLI 子进程） |
| `tools/ota_p0a_drill.sh` | 新增 | 44 项断言的沙盒演练（8 节）：bootstrap 接管、正常升级、健康失败回滚、同版本幂等重装、10 个崩溃点判定、GC 跳过、损坏拒绝、首次安装 |
| `tools/weaknet-local-update.sh` | 修改 | 所有指针/状态写入改走控制器；`cmd_status` 增加 transaction 视图；GC 委托控制器 |
| `tools/ci.sh` | 修改 | 部署控制器到 `/usr/local/lib/weaknet/`、建 `/var/lib/weaknet-ota`、bootstrap 老设备；控制器缺失则部署失败 |
| `.github/workflows/ci.yml` | 修改 | 在 `build-and-test` 作业加一道门禁：跑控制器单元测试 + 沙盒演练 |
| `docs/OTA固件更新与安全发布指南.md` | 修改 | 升 v1.3：新增 §1.1–§1.3（架构/状态转换表/关键不变量）、§3.0（事务查询与人工修复）、§5.1–§5.2 |
| `CLAUDE.md` | 修改 | OTA 意图表补 3 条（查事务状态 / 判定崩溃点 / 事务自测）+ P0-A 边界说明 |

### 关键决策

1. **`--commit` 映射为新终态 `FORCED_COMMITTED`**，与 `COMMITTED` 区分。
   审计上能分辨"真的健康验证通过"与"人说了算"。
2. **`RECOVERY_REQUIRED` 有显式退出通道**：`recover --accept <version>` 或
   `recover --abort`。不允许"自动恢复"——那会把未验证的猜测写回权威状态。
3. **`package` 子命令留在 shell 侧**：它在开发机上跑，开发机没有控制器。
4. **控制器不做开机自启**：P0-A 只提供 `reconcile --dry-run` 供 P0-B 调用，
   自身不在开机时执行任何破坏性动作。
5. **`.previous` 语义收敛为「上一稳定版本」**：旧语义是「上一次操作之前的
   版本」，来回弹跳。派生自权威状态，由控制器维护。

### 演练暴露的真缺陷（都已修 + 加了回归测试）

| 缺陷 | 根因 | 修法 |
|------|------|------|
| 版本号 `"1.0.0\n"` 通过校验 | Python 的 `$` 匹配「结尾换行之前」；版本号会被拼进 `releases/<ver>` 路径 | 正则改 `\Z`（只匹配真正末尾） |
| 回滚后丢失「上一稳定版本」 | `begin` 未传 `--previous-stable-before` 时事务存 `None`，`rolled-back` 写回 `None` | `_previous_stable_before()` 显式回落到状态值 + 回归测试 |
| 同版本重装后 `previous == current` | `commit` 无条件写 `previous = from_version` | 同版本时**不改** previous（否则无参回滚会以「目标与当前相同」失败） |
| bootstrap 把 current 定成 previous | 照抄旧 `.previous` 文件（语义不同，可能等于 current）；后来又发现「版本号最高的已转正版本」在人工回滚后同样不对 | 改为**按证据强度分层推断**并记录 `source`：`transaction_history`（最可信）→ `current` → `committed_mtime` → `previous_file`；证据不足明确返回 `unknown`，不猜 |
| GC 少留一个历史版本 | `keep_count >= max_keep` 少删一个 | 改为 `keep_count > max_keep`（`max_keep` = 额外保留的未受保护版本数） |
| 首次安装装不上第一个版本 | `arm-switch` 要求 `current == from_version`，但新设备根本没有 `current` | 放行「无 current 且 from == to」的首次安装例外（`arm-switch` 与 `reconcile` 同步） |
| 包内旧脚本会覆盖稳定路径 | OTA 包内脚本可能是旧版本（包按某个 commit 构建），旧脚本裸改软链 | `install_self_script` 加控制器感知门禁：不认识 `WEAKNET_OTA_CTL` 的脚本不得接管稳定路径 |
| 演练 5 个崩溃点判定误报 | `check_crash()` 只改 `phase`，残留上一笔事务的 from/to | 每个崩溃点重写整块 transaction 事实 |

### 验证手段

**本地（x86 宿主机）**

```
python3 tools/test_weaknet_ota_ctl.py   → Ran 74 tests, OK
bash tools/ota_p0a_drill.sh             → PASS=44 FAIL=0
bash -n tools/{weaknet-local-update.sh,ci.sh,ota_p0a_drill.sh} → SYNTAX_OK
```

**CI 门禁**（`.github/workflows/ci.yml` 的 `build-and-test` 作业）

```
- name: OTA 事务控制器单元测试与沙盒演练
  run: |
    python3 tools/test_weaknet_ota_ctl.py
    bash tools/ota_p0a_drill.sh
```

控制器是升级可靠性的唯一支点，它坏了整个 OTA 会静默退化——所以这道门禁
不能靠"记得手跑"。

**真机（Radxa Cubie A7A，aarch64，Python 3.9.2，ext4 on /dev/sda3）**

- `renameat2(RENAME_EXCHANGE)` 在板端实测可用（rc=0，两目录内容对调）——
  这决定了同版本幂等重装能否走原子交换路径
- 74 项单元测试在板端 Python 3.9 上**全绿**（不是"宿主机过了就算"）
- 44 项沙盒演练断言在板端**全绿**
- 控制器实装到 `/usr/local/lib/weaknet/weaknet-ota-ctl`（root:root 0755），
  状态目录 `/var/lib/weaknet-ota`（root:root 0700，`transaction.json` 0600）
- bootstrap 接管真机：`stable_version=1.1.0`、`previous_stable_version=1.0.0`、
  无进行中事务；`.previous` 投影由 `1.1.0`（旧语义）纠正为 `1.0.0`
- 事务状态查询与 `reconcile --dry-run` 真机均正常返回
- 迁移前后基线不变：`readlink current` = `releases/1.1.0`；
  `weaknet-cli update status` = `installed_version=1.1.0, current_committed=true`

**真机临时文件已清理**（`/tmp/p0a`、`/tmp/wn-p0a-*`、`/tmp/ra2` 全部删除）。

## 结果

### 已完成

- 事务控制器、单元测试、沙盒演练、升级脚本接入、部署链路、文档同步全部落地
- 真机部署 + 真机验证通过（74+44 项测试在板端全绿，基线不变）
- 关键不变量已固化为可执行断言（失败候选不进 previous / 不做回滚目标；
  同版本重装不改 previous；GC 只在终态运行）

### 遗留（**明确未做，不得宣称完成**）

1. **P0-B：开机自动恢复** —— 独立 systemd 恢复服务 + 最小 SSH 救援模式。
   **已于同日完成**，见 [[2026-10-22-ota-p0b-boot-recovery]]。
2. **真正的掉电测试未做** —— `fsync` 保证的是「内核 → 设备」，不保证设备写
   缓存已落到介质；板端根分区是 `/dev/sda3`（SATA/USB），若该盘启用写缓存
   且未走 FUA，掉电仍可能丢失已 fsync 的数据。当前验证全部用 `kill -9` /
   状态构造代替，**不等于掉电验证**。
3. **云端侧尚无事务状态订阅通道** —— 状态可查，但云端拿不到。
4. **人工修复路径已在真机走过一次** —— 板端注入 TRIAL 故障 → 恢复服务进入
   救援模式 → `require-recovery` + `recover --accept 1.1.0` → 服务恢复
   active、基线不变（`installed_version=1.1.0, current_committed=true`）。
   这次演练暴露了 `recover --accept` 会把 `previous_stable_version` 写成
   `None` 的缺陷，已修并加回归测试。
5. **密钥轮转建议仍然有效**（见 [[2026-10-22-ota-release-pipeline-closure]]）：
   私钥材料曾出现在会话 JSONL 里，正式对外发布前应轮转。

### 后续是否值得升格为正式 OpenSpec change

P0-B 已按工作流外实施交付（见 [[2026-10-22-ota-p0b-boot-recovery]]）。
它新增了一个开机自启的 systemd 单元与对外可见的救援行为，属于真正的产品
surface；若要继续推进（云端订阅通道、掉电测试台），应走完整 Planner →
Generator → Evaluator 流程。
