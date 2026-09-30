# Phase 2 技术方案：设备链路基线与 LinkDegraded 事件

> **定位**：属于《产品定位：工业现场无线信号检测与诊断网关》的**链路层能力**
> （回答："这台设备为什么出现通信异常？信号是在逐渐恶化还是突然消失？"）
> **前置依赖**：Phase 1 + 1b 已交付（Canonical Device Event 结构、`device_events` 表、
> D-Bus / CLI / 查询工具出口）。本阶段不改已有事件模型，只产出新事件与填充新事实。
> **状态**：Brainstorming 设计定稿，待用户最终确认后转入 plan 模式。

---

## 一、三道灵魂拷问（项目专属评审）

### 1. D-Bus 接口契约
- **不新增 D-Bus 方法/信号**：`LinkDegraded` / `LinkRecovered` 直接进入已有的
  `device_events` 表，通过已有的 `QueryDeviceEvents`（`--type LINK_DEGRADED`）
  与 `weaknet-cli events` 取回。
- **总线策略无需改动**：`tools/com.example.WeakNet.conf` 采用接口通配策略
  （`<allow send_interface="com.example.WeakNet"/>`）。
- **向后兼容**：调用 `QueryDeviceEvents` 时若不带 `--type` 自动返回所有类型事件。

### 2. 硬件与环境依赖
- **纯用户态逻辑，不碰内核，不写 eBPF**：
  RSSI 数据来自已有的 BlueZ D-Bus 设备发现路径（`BtMonitor::refreshDeviceStates()`），
  断连事件来自已有的 Phase 1 eBPF 探针（`BtEventMonitor`）。
- **x86 本地可通过单元测试 100% 闭环**：算法、基线更新、滞回状态机、时序回填、
  SQLite 落库全部写 mock/直接构造测完。
- ARM64 / 真机只做冒烟验证（确认在板端真实运行不崩溃、资源开销可控）。

### 3. 多线程并发安全
- **数据流拓扑**：
  - 写入方 1（RSSI 采样）：`bt_monitor` 线程（每 3s 刷新一次设备，调用 `feedRssi()`）
  - 写入方 2（断连通知）：`bt_events` 消费线程（发生断连时调用 `getRecentRssi()` 取回填）
  - 读取方 3（查询）：D-Bus 工作线程 / CLI 查询线程
- **保护边界**：新建 `BtLinkQualityTracker`，内部使用独立的 `std::mutex mutex_`；
  **不借用** `WeakNetMgr::iface_mutex_`，**不借用** `BtMonitor::deviceMutex_`，
  避免锁顺序嵌套导致死锁。

---

## 二、详细技术设计

### 1. 纯相对基线算法（用户拍板：不设统一绝对阈值）

每个设备（以 `device_address` 为主键）独立维护自身基线，适应工业现场不同天线、
不同安装位置的物理差异。

```
                    ┌────────────────────────┐
                    │ 环形样本窗口 (上限 30)   │
                    └───────────┬────────────┘
                                │
                    ┌───────────▼────────────┐
                    │  基线中位数 baseline_dbm│  (需 >= 10 个样本才可信)
                    └───────────┬────────────┘
                                │
                   连续 3 个新鲜样本 < baseline - 15dBm ?
                                │
                  ┌─────────────┴─────────────┐
               YES│                           │NO
                  ▼                           ▼
          触发 LINK_DEGRADED            保持 NORMAL
                  │
        回升至 > baseline - 8dBm ? (滞回防抖)
                  │
                  ▼
          触发 LINK_RECOVERED
```

- **基线统计量**：采用**中位数**而非均值，天然抗毛刺干扰（比如设备偶尔被叉车挡一下产生的单点深跌）。
- **新鲜样本窗口**：仅统计最近 30 个有效样本（`rssi != 0 && rssi > -1000`）。
- **基线收敛门槛**：样本数 `< 10` 时处于 `LEARNING` 状态，不产出任何劣化事件，
  避免设备刚上线时基线未收敛产生误报。
- **滞回设计（Hysteresis）**：
  - 劣化下限：`baseline_median - 15 dBm`，且要求**连续 3 个样本**突破
  - 恢复上限：`baseline_median - 8 dBm`（保留 7 dBm 缓冲带），避免在临界点反复横跳生成事件风暴
- **边沿触发（Edge Triggered）**：状态在 `NORMAL → DEGRADED` 时发射一条 `LINK_DEGRADED`；
  在 `DEGRADED → NORMAL` 时发射一条 `LINK_RECOVERED`。持续处于 DEGRADED 状态不重复发事件。

### 2. 断连前有效 RSSI 的语义与时间窗回填

解决 Phase 1 留白的 `rssi_at_event_dbm = NULL` 问题：

- **断连瞬间的 RSSI 不可测**：断连发生时，物理链路已经中断，无法发起读取；
  即使 BlueZ 缓存里有值，也可能是数分钟前的陈旧快照。
- **有效时间窗（TTL）**：取**断连事件发生时刻前 10 秒内**的最后一次有效 RSSI 采样。
  - 若 `now_ms - last_rssi_ts_ms <= 10000`：填入该数值（真实链路状态）。
  - 若超出 10 秒（如设备早已悄悄静默、超时才报断开）：保持 `NULL`，
    并在 `details_json` 中记录 `{"rssi_stale": true, "last_seen_age_ms": ...}`。
- **事实与推断分离**：回填的是"断开前的真实观测物理量"，不是推断值。

### 3. 数据结构设计（`server/include/wireless_event.hpp` 扩展）

不破坏 Phase 1 结构，仅追加字段与轻量状态结构：

```cpp
/// 设备链路基线画像（轻量版）
struct DeviceLinkProfile {
    std::string device_address;
    BtAddressType address_type = BtAddressType::Unknown;
    int16_t baseline_rssi_dbm = -1000;    ///< 当前基线中位数
    int16_t min_seen_rssi_dbm = -1000;    ///< 历史观测最低值
    int16_t max_seen_rssi_dbm = -1000;    ///< 历史观测最高值
    size_t sample_count = 0;              ///< 有效采样总数
    uint64_t first_seen_ms = 0;           ///< 首次发现时间
    uint64_t last_seen_ms = 0;            ///< 最近活跃时间
    uint32_t degraded_count = 0;          ///< 历史劣化累计次数
    uint32_t disconnect_count = 0;        ///< 历史断连累计次数
    bool is_degraded = false;             ///< 当前是否处于劣化状态
};
```

### 4. 数据库持久化（`device_baselines` 表）

在 `database_manager.cpp` 中新增一张轻量表：

```sql
CREATE TABLE IF NOT EXISTS device_baselines (
    device_address      TEXT PRIMARY KEY,
    address_type        TEXT DEFAULT 'UNKNOWN',
    baseline_rssi_dbm   INTEGER DEFAULT -1000,
    min_seen_rssi_dbm   INTEGER DEFAULT -1000,
    max_seen_rssi_dbm   INTEGER DEFAULT -1000,
    sample_count        INTEGER DEFAULT 0,
    first_seen_ms       INTEGER NOT NULL,
    last_seen_ms        INTEGER NOT NULL,
    degraded_count      INTEGER DEFAULT 0,
    disconnect_count    INTEGER DEFAULT 0,
    is_degraded         INTEGER DEFAULT 0,
    updated_at          INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_device_baselines_updated ON device_baselines(updated_at);
```

- **落库策略**：内存中由 `BtLinkQualityTracker` 维护，每当状态变化（进入劣化/恢复）
  或周期持久化（与 `history_thread` 5s 节拍对齐，按 dirty 标记写）落盘。
- **启动回放**：服务启动时预热加载最近 100 台已知设备的基线，避免重启后重新经历 10 个采样的静默期。

### 5. 查询与工具扩展
- `history_query_tool --baselines`：输出全部/指定设备的链路画像 JSON。
- `weaknet-cli events --type LINK_DEGRADED`：直接生效，无需改 CLI 代码（Phase 1b 已支持按类型过滤）。

---

## 三、文件清单与修改范围

| 动作 | 文件 | 内容 |
| ---- | ---- | ---- |
| **新增** | `server/include/bt_link_quality_tracker.hpp` | 基线计算器、滞回状态机、TTL 检索器 |
| **新增** | `server/src/bt_link_quality_tracker.cpp` | 实现：中位数滑动窗口、边沿事件发射、线程安全保护 |
| **新增** | `server/test/unit/test_bt_link_quality_gtest.cpp` | 单元测试：基线收敛、突降触发、滞回恢复、TTL 超时置 NULL |
| **修改** | `server/include/wireless_event.hpp` | 引入 `DeviceLinkProfile` 结构与序列化方法 |
| **修改** | `server/src/wireless_event.cpp` | `DeviceLinkProfile::toJson()` 实现 |
| **修改** | `server/include/database_manager.hpp` | `insertOrUpdateBaseline()` / `queryBaselines()` |
| **修改** | `server/src/database_manager.cpp` | 创建 `device_baselines` 表及对应 CRUD |
| **修改** | `server/src/bt_monitor.cpp` | 在 `refreshDeviceStates()` 采集到新 RSSI 时喂给 `BtLinkQualityTracker` |
| **修改** | `server/src/bt_event_monitor.cpp` | 在断连发生时向 tracker 查询过去 10 秒的最近有效 RSSI，注入事件 |
| **修改** | `server/src/history_query_tool.cpp` | 新增 `--baselines` 查询参数 |
| **修改** | `server/test/CMakeLists.txt` | 注册新测试 `test_bt_link_quality_gtest` |

---

## 四、验证设计

1. **单元测试（x86，100% 覆盖核心逻辑）**：
   - 样本数 `<10`：即使灌入 `-95dBm` 也不触发劣化（基线未收敛保护）
   - 样本数 `>=10`（基线稳定在 `-60dBm`）：
     - 注入单次 `-80dBm`：不触发（抗瞬时毛刺）
     - 连续注入 3 次 `-80dBm`（低于基线 20dBm）：准确触发 1 条 `LINK_DEGRADED` 事件
     - 注入 `-69dBm`（位于 -15dBm 与 -8dBm 滞回区之间）：不触发恢复
     - 注入 `-65dBm`（高于基线 -8dBm）：准确触发 1 条 `LINK_RECOVERED` 事件
   - 断连时间窗验证：
     - 5 秒前有有效 RSSI `-65dBm` → 断连事件 `rssi_at_event_dbm = -65`
     - 15 秒前有有效 RSSI → 断连事件 `rssi_at_event_dbm = null`（超时保护）
2. **端到端开发板回归**：
   - ARM64 交叉编译 + `./tools/ci.sh` 一键部署
   - 开发板冒烟测试保持全绿（PASS 22）
