# Phase 3a 技术方案：SiteIncident 与区域级异常关联

> **定位**：属于《产品定位：工业现场无线信号检测与诊断网关》的**区域层能力**
> （回答："是一台设备坏了，还是这一片无线环境出了问题？影响了哪些设备？"）
> **前置依赖**：Phase 1 / 1b（Canonical Device Event + `device_events` 表 + 查询出口）、
> Phase 2（RSSI 基线 + `LinkDegraded` / `LinkRecovered`）。本阶段不改事件模型，
> 只在事件流之上新增一层关联。
> **状态**：已实现（x86 单测 + ARM64 容器 + 真机部署验证通过）。

---

## 一、三道灵魂拷问（项目专属评审）

### 1. D-Bus 接口契约
- **新增一个只读方法** `QuerySiteIncidents`（`state, start_ms, end_ms, limit` → JSON）。
  纯增量，不改动任何既有方法签名。
- **总线策略无需改动**：`tools/com.example.WeakNet.conf` 采用接口通配策略。
- **向后兼容**：不接关联器的 `WirelessEventStore` 行为与本阶段之前完全一致。

### 2. 硬件与环境依赖
- **纯用户态逻辑，不碰内核，不写 eBPF**：关联器的输入就是 `device_events` 表
  里已有的 canonical 事件。
- x86 单元测试可 100% 闭环：门槛判定、幂等、生命周期、重启回放、证据回链全部
  在单测里构造时间线验证（16 项）。ARM64 / 真机只做部署与查询出口冒烟。

### 3. 多线程并发安全
- **数据流拓扑**：
  - 写入方：`bt_events` 消费线程（`BtEventMonitor::flushToStore` 之后）
  - 读取消：D-Bus 工作线程 / CLI 查询
- **保护边界**：`SiteIncidentCorrelator` 内部独立 `mutex_`，不借用 store 或
  BtMonitor 的锁。全局加锁方向固定为
  `（无）→ correlator.mutex_ → DatabaseManager::mutex_`。

---

## 二、详细技术设计

### 1. 为什么 Incident 必须独立建模

`DeviceEvent` 是**不可变事实**（一次物理断连只有一条），`SiteIncident` 是
**持续解释**（"这一片区域此刻有问题"有开始、有延续、有结束）。两者生命周期不同，
必须分开建模：把若干 event 打上同一个 tag 无法表达"事故何时结束"。

```
WirelessDeviceEvent（设备层事实，不可变）
     ↓ SiteIncidentCorrelator::observe()
SiteIncident（区域层解释，OPEN → ONGOING → RESOLVED）
     ↓
site_incidents 表  +  site_incident_events 回链（1:N）
```

### 2. 合格异常判定（`QualifyingAnomalyPolicy`）与阈值可配

五个现场阈值经由 `monitors.bluetooth` 配置块传入（唯一事件生产者当前是蓝牙
链路，且 `bt_events` 与 `bluetooth` 共用 `enabled` 开关）：

| 配置键 | 默认值 | 范围 | 关联器字段 |
| ------ | ------ | ---- | ---------- |
| `incident_min_devices` | 2 | 2 ~ 1000 | `min_affected_devices` |
| `incident_min_ratio_bp` | 3000（30%） | 1 ~ 10000（万分比） | `min_affected_ratio` |
| `incident_window` | 60s | 1s ~ 10m | `correlation_window_ms` |
| `incident_quiet_window` | 60s | 1s ~ 24h | `quiet_window_ms` |
| `incident_active_window` | 60s | 1s ~ 10m | `active_device_memory_ms` |

- **生效语义**：关联器**构造期**读入。运行时 `weaknet-cli set` 后需
  `monitor restart bt_events` 重建才生效；重启回放会恢复既有事故，不丢状态。
  因此这些键刻意不进 TRIAL 白名单（与 `map_sizing` 同理：试改成功但无法
  回滚生效会给出假象）。
- **格式**：比例用万分比（bp）而不是浮点——与 `map_sizing_ram_budget_bp`
  同一惯例，避免 D-Bus argv / CLI 传浮点的 locale 歧义。

阈值之外，"哪些事件算合格异常"是独立的判定语义：

| 事件 | 是否合格 | 依据 |
| ---- | -------- | ---- |
| `LinkDisconnected` + `ConnectionTimeout` / `AuthenticationFailure` / `Other` / `Unknown` | ✅ | 非计划断连 |
| `LinkDisconnected` + `RemoteUserTerminated` / `LocalHostTerminated` | ❌ | 计划内断开（用户关机/主动断连） |
| `LinkDegraded` | ✅ | 链路层异常本身 |
| `DeviceAppeared` / `DeviceLost` / `LinkConnected` / `LinkRecovered` / `ConnectionAttempt` | ❌ | 非异常事实 |

两个刻意的决定：

- **排除计划内断开**：若计入，每天下班集体关机必然误报一起"区域无线故障"。
  注意 mgmt 与 HCI 两个 reason 域不可混用（`0x02` 在 mgmt 域是本地断开、
  在 HCI 域是 Unknown Connection Identifier），归一化必须在 `bt_event_normalizer`
  已完成的语义之上做。
- **不排除 `Unknown`**：语义是"已确认断开但原因不在已知分类内"。未知不是正常，
  排除它会让故障静默消失（与 `wireless_event.hpp` 中"Other/Unknown 不能成为
  信息黑洞"同一条纪律）。

### 3. 双门槛与动态分母（本阶段最关键的设计）

```
                    ┌──────────────────────────────┐
                    │ 窗口内合格异常 → 受影响设备集合 │
                    └───────────────┬──────────────┘
                                    │
        ┌───────────────────────────┴───────────────────────────┐
        │  门槛一：affected_devices >= min_affected_devices (2)   │
        │  门槛二：affected / active_devices >= ratio (0.30)      │
        └───────────────────────────┬───────────────────────────┘
                                 达标 → 开 SiteIncident
```

**分母 `active_devices` 是动态的**：只统计"活跃设备记忆"里仍然新鲜的设备
（出现过**任一**事件，含计划内断开与非异常事件），记忆时长 `active_device_memory_ms`。

为什么不能用全库设备数：工业现场的设备画像长期留存，一台三个月前离线、再未出现的
设备会把比率永久稀释到永不达标——关联器**静默失效且没有任何报错**。这是实现时最容易
退化成静态分母的地方，单测 `DenominatorIsActiveDevicesNotKnownDevices` 专门守住它。

两个门槛必须同时满足。**只看比例**会让单设备抖动直接升级为区域事故；
**只看绝对数**会让 2/2 与 2/200 得到相同结论。

### 4. 时间语义：事件驱动，不做轮询

- 关联窗口、静默期全部以 `WirelessDeviceEvent::timestamp_ms`（墙钟毫秒）为准。
  关联器**不在内部读时钟**——读时钟会把判定结果绑到调用节奏上，也让单测无法
  确定性地构造时间线。
- `started_at_ms` 取窗口内**最早**一条异常（现实事故从最早异常就开始，不是
  "达标那一刻"）；`last_event_ms` 只右移，绝不因迟到事件回退。
- `resolved_at_ms = last_event_ms + quiet_window_ms`（静默期满才敢说结束）。
- 结案由消费线程每轮 `flushToStore` 之后的 `tickIncidents()` 推进：
  静默期内没有新事件，也就没人调用 `observe()`，"没有新异常"必须同样能推动状态。

### 5. 幂等与重启连续性（两个必须落库的理由）

**(a) 同一 `event_id` 只计一次。** `recordEvent` 可能因重试/回放被重复调用；
重复事件会把受影响设备数灌水到门槛之上，凭空造出一起假事故。

**(b) `incident_id` 是确定性的**：`sitinc_<site>_<started_at_ms>`，刻意**不带**
进程实例随机码——这与 `device_events.event_id` 的策略**相反**。
原因：重启回放会重新推导出同一份 incident，必须写回同一行（UPSERT）而不是插新行，
否则"重启一次多一起事故"。`device_events.event_id` 必须带实例码，是因为它是
`UNIQUE` + `INSERT OR IGNORE`，跨重启撞键会被静默丢弃——两者是不同约束下的
不同解法，不要互相套用。

**回放策略**：启动时重放最近 `correlation_window + quiet_window` 内的 `device_events`，
并以下限 `queryLatestIncidentResolvedAt(site_id)` 排除已结案区间，然后重新推导活跃
incident。回放**先于** `setIncidentCorrelator()` 绑定执行，避免回放自身又触发一轮关联。

### 6. 数据库设计

```sql
CREATE TABLE IF NOT EXISTS site_incidents (
    incident_id TEXT PRIMARY KEY, site_id TEXT, gateway_id TEXT,
    started_at_ms INTEGER, last_event_ms INTEGER,
    resolved_at_ms INTEGER DEFAULT NULL,     -- NULL = 仍在活跃，不是 0
    affected_devices INTEGER, state TEXT,
    suspected_cause TEXT DEFAULT NULL        -- Phase 3a 恒 NULL
);
CREATE TABLE IF NOT EXISTS site_incident_events (
    incident_id TEXT NOT NULL, event_id TEXT NOT NULL,
    PRIMARY KEY (incident_id, event_id)      -- INSERT OR IGNORE 使回放幂等
);
```

- **受影响设备清单不存第二份**：由回链表 JOIN `device_events` 派生，
  避免同一事实两个来源。
- **不建 FOREIGN KEY**：`device_events` 有保留期清理，强制外键会让清理失败或
  级联删除事故证据。清理顺序由代码保证（先删回链、再删 incident）。
- 时间过滤以 `last_event_ms` 为准：跨 3 点的事故应当落进 3 点这个窗口，
  而不是因为开始于 2:58 而消失。

### 7. `suspected_cause` 恒为 NULL —— 这是刻意承诺，不是缺陷

关联器只回答"有没有多台设备同时段异常、分别是谁、证据是哪几条"，**绝不回答"为什么"**。
根因推断属于诊断器（Phase 4+）。一次"因为没有证据就断言是 Wi-Fi 干扰"的误判，
足以毁掉长期可信度。

---

## 三、文件清单与修改范围

| 动作 | 文件 | 内容 |
| ---- | ---- | ---- |
| **新增** | `server/include/site_incident.hpp` | `IncidentState` / `QualifyingAnomalyPolicy` / `SiteIncident` |
| **新增** | `server/src/site_incident.cpp` | 枚举转换、合格异常判定 |
| **新增** | `server/include/site_incident_correlator.hpp` | 关联器接口与配置 |
| **新增** | `server/src/site_incident_correlator.cpp` | 双门槛、幂等、生命周期、重启回放 |
| **新增** | `server/test/unit/test_site_incident_gtest.cpp` | 16 项验收测试 |
| 修改 | `server/include/database_manager.hpp` / `src/database_manager.cpp` | 两张表 + UPSERT/回链/查询/回放/清理 |
| 修改 | `server/include/wireless_event_store.hpp` / `src/wireless_event_store.cpp` | 绑定关联器、落库后投递、tick 代理、查询代理 |
| 修改 | `server/src/bt_event_monitor.cpp` / `include/bt_event_monitor.hpp` | 消费循环推进静默期 |
| 修改 | `server/src/monitor_ebpf_plugins.cpp` | 关联器所有权 + 回放早于绑定 |
| 修改 | `server/include/server.hpp` | `ServerContext::site_incident_correlator` |
| 修改 | `server/include/wireless_event.hpp` / `src/wireless_event.cpp` | 回放所需的两个 `fromString` 逆映射 |
| 修改 | `server/include/common.hpp` / `src/dbus_service.cpp` / `include/dbus_service.hpp` | `QuerySiteIncidents` 方法 |
| 修改 | `client/weaknet_client.h` / `client.cpp` | C API `weaknet_query_site_incidents` |
| 修改 | `client/weaknet_cli.cpp` | `weaknet-cli incidents` |
| 修改 | `server/src/history_query_tool.cpp` | `--incidents` / `--state` |
| 修改 | `server/test/CMakeLists.txt` | 注册 `test_site_incident_gtest` |
| 修改 | `server/include/weaknet_config.hpp` / `src/weaknet_config.cpp` | 五个 `incident_*` 配置键（yaml / set / get / serialize 四路径 + 范围校验） |
| 修改 | `config.yaml` | `monitors.bluetooth` 下的 incident 默认值与说明注释 |

---

## 四、验证设计

1. **单元测试（x86，20 项，全绿）**：
   - `SingleDeviceAnomalyDoesNotOpenIncident` —— 单设备异常不触发
   - `MultipleDevicesOpenIncidentAndProgressLifecycle` —— 开事故 + OPEN→ONGOING→RESOLVED
   - `PlannedDisconnectDoesNotContributeEvidenceButCountsAsActive` —— 正常断开不计入
   - `LinkDegradedCountsAsAnomaly` —— 劣化算合格异常
   - `DuplicateEventIdIsIdempotent` —— 幂等去重
   - `DenominatorIsActiveDevicesNotKnownDevices` —— 动态分母
   - `RatioGateBlocksWideButSparseFailures` —— 比例门槛
   - `LateEventMovesStartEarlierButNeverRewindsLastEvent` —— 迟到事件：`started_at_ms`
     补记更早、`last_event_ms` 绝不回退
   - `OutOfOrderThresholdEvalUsesEarliestQualifyingEvidence` —— 乱序达阈：
     `started_at_ms`/`last_event_ms` 取窗口极值而不是"本条事件"时刻
   - `UpsertIsIdempotentOnIncidentId` / `ActiveIncidentSerializesResolvedAtAsNull` /
     `StateFilterAndLatestResolvedFloor` / `EvidenceBacklinkIsQueryableAndIdempotent` ——
     持久化与 NULL 语义
   - `ReplayRestoresActiveIncidentWithoutDuplicating` / `ReplayDoesNotResurrectResolvedIncident` ——
     重启连续性
   - `CleanupRemovesBacklinksBeforeIncidentRows` / `CleanupKeepsRecentIncidentsAndTheirBacklinks` ——
     cleanup 先删回链再删本体（无悬垂）且不误删近期事故
   - `StoreForwardsEventsToCorrelator` —— 接线（store → correlator 单一生产消费者）
2. **配置测试（`SiteIncidentConfigTest`，6 项，全绿）**：yaml 加载、默认值与
   关联器默认一致、set/get 回读、越界拒绝且旧值不变、不进 TRIAL 白名单、
   serialize 输出。
3. **回归**：`ctest --test-dir build-x86/server` 45/45 全绿（两个受影响套件
   20 + 55 用例）。
4. **ARM64**：容器内 `cmake --build build-arm64 -j1` 全量通过。
5. **真机**：`BOARD=board ./tools/ci.sh` 部署 + 板端冒烟全绿；
   在既有旧库上完成 schema 迁移（两张新表已建），
   三条查询出口（D-Bus / CLI / 离线工具）返回正确 JSON，含 `resolved_at_ms` 的
   NULL 语义与设备清单回链；`weaknet-cli get bluetooth` 输出五个 `incident_*` 键。

---

## 五、下一阶段（Phase 4+）的接口预留

| 预留点 | 位置 | 用途 |
| ------ | ---- | ---- |
| `SiteIncident::suspected_cause` | 领域模型 + DB 列 + JSON | 诊断器写入根因推断（列已就位，当前恒 NULL） |
| `site_incident_events` 回链 | DB | 诊断器取"这起事故由哪几条证据构成" |
| `gateway_id` | 领域模型 + DB 列 | 多网关组网后区分"是哪个采集点看到的" |
| `QualifyingAnomalyPolicy` | 独立结构体 | 接入 Wi-Fi 环境证据时需要扩展判定（Wi-Fi 当前只是 Environment Evidence Provider） |
