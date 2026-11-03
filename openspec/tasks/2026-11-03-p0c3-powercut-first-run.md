# P0-C3 真实断电验证（首次）

> 日期：2026-11-03
> 分类：工作流外实施（真机断电验证 + 新增测试台脚本）
> 结果：单次通过，**不足以证明断电自愈完成**

## 构思

### 要解决什么问题

P0-C2 验证的是 `reboot`（有序关机）：内核会把页缓存刷到设备，所以它能证明
「状态在正常重启后持久」，**证明不了「掉电时状态是否持久」**。

板端根分区 `/dev/sda3` 的 `/sys/block/sda/queue/write_cache` 是 **`write back`**，
意味着 `fsync` 返回只代表数据交给了设备，不代表设备写缓存已经落到介质。
断电时，仍在设备缓存里的数据会丢失 —— 这正是 P0-C3 要测的东西。

### 为什么不做进 OpenSpec change

验证任务不改产品代码。`tools/ota_p0c3_powercut_drill.sh` 是测试台脚本，
不与任何产品 surface 绑定，也不是运行时依赖。按规则记入 `openspec/tasks/`。

### 备选方案与取舍

| 方案 | 取舍 |
|---|---|
| 生产路径直接断电 | 覆盖最真实，但断电后若状态损坏，生产板停在不可用态。**否决** |
| 沙盒 + 断电 | 隔离生产，验证同一套控制器与持久化路径。**采纳** |
| 改 `write_cache` 为 `write through` 做对照 | 能证明缓存的影响，但会改生产盘的内核参数。**本轮未做** |

## 实现方式

### 测试台

`tools/ota_p0c3_powercut_drill.sh`（225 行，`bash -n` 通过）：

```
sudo bash ota_p0c3_powercut_drill.sh setup <scenario>   # 构造断电时机
sudo bash ota_p0c3_powercut_drill.sh verify            # 断电重启后校验
sudo bash ota_p0c3_powercut_drill.sh status            # 查看当前状态
sudo bash ota_p0c3_powercut_drill.sh cleanup           # 清理沙盒
```

四个断电时机：`trial` / `arm-switch` / `do-switch` / `commit-immediate`。

隔离边界：

- 独立 root `/var/lib/p0c3-sb/root`（生产是 `/home/radxa/weaknet`）
- 独立 state-dir `/var/lib/p0c3-sb/state`（生产是 `/var/lib/weaknet-ota`）
- **不安装任何开机自启单元** —— P0-C2 踩过的坑：沙盒恢复器的
  `enter_rescue()` 会 `service_stop_and_mask()` 与 `ensure_rescue_access()`，
  后者操作的是**真实的** `ssh` / `weaknet-tunnel` 单元名。P0-C3 完全绕开
  恢复器，判定全部由 `verify` 手工驱动。

### 关键实现点

**断电标记必须 `fsync`**：`write_marker()` 写文件后 `fsync(fileno)`，
再 `fsync` 目录 fd。否则标记本身也可能丢，导致无法判断"断电发生在哪一步之后"。

**脚本自身不能放在 `/tmp`**：第一次执行时脚本在 `/tmp`，断电后被
`D /tmp` 规则清空，重新上电后无法运行 `verify`。已改安装到
`/usr/local/bin/ota_p0c3_drill`（持久）。

## 结果

### 首次断电（`trial` 场景）

断电前基线：

```
transaction_id = 20261103T110828Z-6916
phase          = TRIAL
boot_attempts  = 0
revision       = 3
current        -> releases/1.1.0
断电标记       = trial-established
```

断电操作：用户直接拔电源，等待约 5 秒后重新上电。

重启后（新 boot_id `90eb9316-e765-472a-b06e-e0d45aa9fd36`，开机 11:15:05）：

| 项 | 断电前 | 断电后 | 判定 |
|---|---|---|---|
| transaction_id | `20261103T110828Z-6916` | 同 | ✅ 完整 |
| phase | `TRIAL` | `TRIAL` | ✅ 一致 |
| boot_attempts | `0` | `0` | ✅ 一致 |
| revision | `3` | `3` | ✅ 一致 |
| current | `releases/1.1.0` | `releases/1.1.0` | ✅ 一致 |
| 状态可解析 | — | OK | ✅ 未损坏 |
| reconcile | — | `continue_trial` | ✅ 与 phase 自洽 |

生产状态复核：`weaknet-server` / `weaknet-tunnel` / `ssh` 全部 `active`，
OTA 事务 `COMMITTED / 1.1.0`，零影响。

### 沙盒清理

`/var/lib/p0c3-sb` 与 `/usr/local/lib/p0c3-sb` 已删除，无残留。
`/usr/local/bin/ota_p0c3_drill` 保留（测试台，非运行时依赖）。

### 必须说清的局限

**单次通过 ≠ 断电自愈已证明。** 理由：

1. `write back` 的丢数据是**概率性**的。一次成功只说明"这一次设备缓存刚好
   刷下去了"，不说明"永远不丢"。
2. 本轮只测了 `trial` 一个时机，`arm-switch` / `do-switch` /
   `commit-immediate` 未测。
3. 没有做 `write_cache=write through` 的对照实验，因此**无法量化**
   写缓存对丢失窗口的实际影响。
4. 没有做重复断电统计（用户判断一次即可）。

因此本记录的结论边界是：

> **`trial` 场景下，单次真实断电后事务状态完整保留、指针一致、
> `reconcile` 判定正确。** 不能外推为"断电自愈已完成"。

### 后续（若要关闭 P0-C3）

1. 重复 `trial` 场景 N 次（建议 ≥10），统计失败率；
2. 补齐另外三个断电时机；
3. 可选：`write through` / `write back` 对照，量化缓存影响；
4. 若要提升生产可靠性，可考虑对 `/var/lib/weaknet-ota` 所在分区
   单独设 `barrier` 或改用带掉电保护（PLP）的存储介质。
