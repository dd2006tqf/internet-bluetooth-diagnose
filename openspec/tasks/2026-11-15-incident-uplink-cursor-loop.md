# 事故域上行游标死循环：游标推进了却永远追不上自己

- 日期：2026-11-15
- 类型：工作流外实施（未创建 OpenSpec change）
- 触发：用户提问「我们是不是已经完整打通链路了，还差什么错误」后核查上行日志
- 结果：**根因已定位并修复，x86 全量 67/67 通过，板端实测循环停止**

## 构思

### 发现过程

用户问「链路是否已完整打通」。核查板端日志时发现一条异常高的重复率：

```
$ sudo journalctl -u weaknet-server --since "-5min" | grep -c "无线事实上行成功"
976
```

5 分钟 976 条，约每 0.3 秒一次。而 `incidents=1` 去重后只有一个值，
`incidents_ms=1794745808177` 六小时没有变化。

这不是"上行很勤快"，是**同一批数据被反复 POST**。云端收到近千次同样的请求。

### 根因：三处代码各自合理，组合起来成环

```
1. database_manager.cpp  loadSiteIncidentsSince():
       WHERE last_event_ms >= ?          ← 用 >=
2. edge_wireless_uplink_exporter.cpp  collect():
       db_.loadSiteIncidentsSince(incidents_ms, ...)   ← 传游标本身（事件域传的是 +1）
3. edge_wireless_uplink_exporter.cpp  advanceCursors():
       after.incidents_ms = max(after.incidents_ms, inc.last_event_ms)  ← 原地不动
4. edge_wireless_uplink_exporter.cpp  run():
       成功且 collect() 仍非空 → continue    ← 跳过 interval 等待，立刻重跑
```

合起来就是：

```
发出去 → 游标推进到 last_event_ms → 因为用 >=，同一行又被捞回
      → advanceCursors 把游标设成同一个值 → run() 看到"还有积压"
      → continue → 回到第一步
```

`interval` 下限 1000ms 的保护完全没起作用，因为根本走不到等待逻辑。

### 为什么原有测试没抓到

`test_edge_wireless_uplink_gtest.cpp` 的 `UsesIndependentPerDomainCursors`
用的是 `incidents_ms = 0` 和 `5000` 两个值，而该测试里事故的
`last_event_ms = 4000` —— **两个值都不等于 4000**，恰好绕过了边界。
它验证了"三域游标互不干扰"，但没验证"游标能收敛"。

这是典型的**判据只覆盖了正常路径**：`0` 和 `5000` 都能区分"是否用了本域游标"，
但对"游标等于本域最大值时会怎样"完全没有区分能力。

### 三域对照，问题只在一处

| 域 | SQL 条件 | 调用方传参 | 结果 |
| --- | --- | --- | --- |
| 事件 | `ts >= ?` | `events_ms + 1` | 正确（严格大于语义） |
| 基线 | `updated_at > ?` | `baselines_ms` | 正确（严格大于语义） |
| **事故** | **`last_event_ms >= ?`** | `incidents_ms` | **死循环** |

只有事故域两个坑都踩了：SQL 是 `>=`，调用方又没 `+1`。
`268bc0c` 那次「三域独立游标」的重构里，事件域加了 `+1`、基线域天然用了 `>`，
事故域被漏掉了。

## 实现方式

### 修法：`>=` 改 `>`

```sql
-        WHERE last_event_ms >= ?
+        WHERE last_event_ms > ?
```

选这个而不是"调用方 +1"的理由：

`last_event_ms` 是**毫秒时间戳**。用 `+1` 会跳过落在同一毫秒内的其它事故
—— 而一起区域事故本就可能在同一毫秒内演化出多条。基线域用的也是 `>`，
改 `>` 还能让两个域语义一致。

### 原意图是否被破坏

原注释写着「一起仍在吸收证据的事故会被反复重发（云端 UPSERT 幂等）」。
改 `>` 之后这个意图**仍然成立**，因为：

`upsertSiteIncident` 的 SQL 里有

```sql
ON CONFLICT(incident_id) DO UPDATE SET
    last_event_ms = excluded.last_event_ms, ...
```

新证据到达时 `last_event_ms` 会在数据库里前进，下次自然重新越过 `>`。
而**没有**新证据时本来就不该重发 —— 那正是 `>=` 与 `>` 的分界。

### 新增测试（RED → GREEN）

`EdgeWirelessUplinkCollect.IncidentCursorAtWatermarkYieldsNoNewFacts`：

1. 插入一条 `last_event_ms = 4000` 的事故
2. 游标从 0 开始 `collect()` → 断言捞到 1 条
3. 调 `advanceCursors` 得到新游标（应为 4000），`storeCursors`
4. **再次 `collect()` → 断言返回 `false` 且 `error == "no new facts"`，`
   `incidents` 为空**

第 4 步是关键。修复前它返回 `true` 且带着同一条事故 —— 这个断言直接把
「游标推不动」变成可判定的失败，而不是靠人看日志频率。

RED 验证（改回 `>=` 时）：

```
Value of: second.incidents.empty()
  Actual: false
Expected: true
事故被重复捞回：游标 4000 与该行 last_event_ms 相等时仍命中 SQL 的 >=
```

GREEN 后：该套件 14/14，x86 全量 67/67。

### 板端实测（这是唯一有效的验收）

| 指标 | 修复前 | 修复后 |
| --- | --- | --- |
| 每分钟上行条数 | **188**（约 0.3s/次） | **3**（约 20s/次） |
| `incidents` 字段 | 恒为 `1`（重复发同一条） | `0`（已确认，不再发） |
| `incidents_ms` | 六小时不变 | 保持不变（**正确**：没有新事故就没有新事实） |
| `baselines_ms` | —— | **递增**（1794749802 → 1794749875），证明游标仍在正常推进 |

注意 `incidents_ms` 不变这次是**预期行为**，与修复前的"推不动"形态相同但成因相反：
修复前是循环里反复捞回同一行，修复后是确实没有新事故。

## 结果

### 改了什么

| 文件 | 改动 |
| --- | --- |
| `server/src/database_manager.cpp` | `loadSiteIncidentsSince` 的 `>=` 改 `>`，并把取舍写进注释 |
| `server/test/unit/test_edge_wireless_uplink_gtest.cpp` | 新增 1 项测试钉住游标收敛 |

### 验证

- RED：改回 `>=` → 新测试精确失败（游标 4000 仍命中）
- GREEN：该套件 14/14
- 回归：x86 全量 67/67
- ARM64 交叉编译通过
- 板端部署（releases/1.1.0 转正）后实测：188 条/分 → 3 条/分

### 遗留与边界

- **没有验证云端去重是否掩盖了这个 bug 的更早迹象**。云端对事故是 UPSERT 幂等，
  所以近千次重复 POST 不会产生脏数据，但会消耗带宽与云端 CPU。这部分开销
  没有量化。
- **`run()` 里"成功且仍有积压就 continue"这个模式本身仍有风险**：它依赖
  `collect()` 能收敛。事件域和基线域目前都收敛，但若将来新增第四个域
  又踩同样的 `>=`，会以完全相同的方式成环。已在本记录里点明，
  但没有加防护（比如"连续 N 轮无进展就强制等待"）—— 那属于设计变更，
 不在本次修复范围。
- 该 bug 从 `268bc0c` 存在至今，期间所有上行都在刷屏。影响面仅限
  日志噪音与重复请求，未影响到数据正确性（推送内容是对的，只是发了太多遍）。
