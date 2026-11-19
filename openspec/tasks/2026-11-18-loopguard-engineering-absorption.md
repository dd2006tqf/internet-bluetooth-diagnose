# 从 LoopGuard 吸收三项工程能力（T1 引用闭合 / T2 回执持久化 / T3 慢查询卸载）

> 日期：2026-11-18
> 分类：工作流外实施
> 结果：已完成（T1/T2/T3 全部实现、回归通过、ARM64 交叉编译通过）；T4 已评估但主动推迟

## 构思

### 起点

用户提出把另一个项目（LoopGuard：交换机环路检测与保护系统）的**技术栈与
技术能力**选择性融入本项目，而不是做产品合并。两个项目的领域不同
（WeakNet 是工业无线诊断网关，只观测不控制；LoopGuard 是 L2 环路的检测与
保护，会主动隔离端口），因此可吸收的只有工程手法，不是业务语义。

### 筛掉了什么，为什么

LoopGuard 里最显眼的能力是 **AF_PACKET 主动探测帧 + 端口/VLAN 隔离**。
它对 WeakNet 有诱惑力——「发一个探测帧，看它从哪个口回来」听起来像很强的
归因证据。但它改变了产品的权责边界：主动注入探测帧并据此执行隔离，属于
L5 执行层，与 `docs/产品定位-工业无线诊断网关.md` 的「不控制、不管理」
直接冲突。**T4 因此推迟**，等用户先做产品定位决策（见文末）。

留下的是三项与业务语义无关、纯粹「代码本来就该这样」的缺陷修复。

### T1：清理必须做引用闭合

`DatabaseManager::cleanup()` 原本是一条直白的
`DELETE FROM device_events WHERE ts < cutoff`。问题在于
`site_incidents.affected_device_ids` 不是存下来的字段，而是**查询时 JOIN 派生**的：

```sql
SELECT DISTINCT d.device_address
FROM site_incident_events e JOIN device_events d ON d.event_id = e.event_id
```

`site_incident_events` 这张关联表刻意没有 FOREIGN KEY（`device_events` 有保留期
清理，强制外键会让清理失败或级联删掉事故证据），于是没有任何机制阻止清理删掉
仍被存活事故引用的证据事件。一条**跨越保留期边界**的事故（首次异常在 30 天前、
最后异常在今天）在 `--cleanup 7` 之后，影响面会静默变短：现场只剩「影响 1 台
设备」，真实影响的另外两台被删了，且没有任何错误或日志。

这是留证链路里最坏的一种缺陷——**证据被删了，而记录还在，看起来完全正常**。

### T2：两处「重启即失忆」的去重状态

云端的 `pending_actions` 靠客户端侧去重防重放，而触发重放的两大条件恰好是
**回执丢失**与**板端重启**。但两处去重状态都只在内存里：

| 状态 | 位置 | 重启后 | 原有注释的说法 |
|---|---|---|---|
| `last_applied_generation_` | `WeakNetConfig`，裸 `std::atomic` | 归 0 | 无 |
| `executed_action_ids_` | `EdgeTelemetryExporter`，`unordered_map` | 空 | 「进程内即可…重启后重建执行是正确行为」 |

后者对**非幂等动作**（OTA 升级）完全不成立：重启后第一轮上行就把队列里的
OTA 重放一遍，TRIAL 看门狗被重置、action-results 泛洪。

更糟的是 `executed_action_ids_` 满 1024 条时的处理是 `clear()`——**一次性丢掉
全部**，包括最旧的那些，而那些恰恰是回执最可能丢失、最需要防重放的条目。

### T3：只读查询占住 D-Bus 派发线程

2026-11-01 修过一次同类缺陷（派发层排空，25s → 1.3ms），但**处理函数内部**
的阻塞没被覆盖。D-Bus 派发是单线程串行的，四条历史查询直接跑在派发线程上：

x86 满库（10000 行）实测 `queryHistory` p50 = 118ms / p95 = 130ms / 8.5MB JSON。
开发板 A55 单核 3~5 倍 → 数百毫秒。现场终端选 13h 窗口（恰好就是
`kMaxHistoryLimit = 10000`）时，同窗口的 HealthCheck / Ping /
GetNetworkExperience 全部排队，看板上的「当前状态」跟着卡住。

## 实现方式

### T1 — `server/src/database_manager.cpp`

`cleanup()` 从 86 行重写为 127 行，改为单个 `BEGIN IMMEDIATE` 事务内按**语义
顺序**执行：

1. 删过期事故的关联行（`site_incident_events`）
2. 删过期事故本身（`site_incidents`）
3. 删过期 `device_events`，**带引用闭合**
4. 删 `network_history` / `bt_history` / `bt_data_path_metrics`

三个关键决策：

- **顺序是语义的，不是风格**。先删关联行与事故，第 3 步的 `NOT EXISTS`
  闭包才只看得到「存活事故」，不会因为一条半小时后就要被删的事故而永久
  留住证据。
- **用 `NOT EXISTS` 而不是 `NOT IN`**：`site_incident_events.event_id` 若有
  NULL，`NOT IN` 会退化成「什么都删不掉」（SQL 三值逻辑），表现为清理静默失效。
- **cutoff 只算一次**：原实现每条语句都用 `strftime('now')`，跨秒边界时两张表
  的截止点会差一秒——测试里偶发、现场无从复现的那类缺陷。

另外 `retention_days < 0` 直接从「落到未来、清空整库」改为拒绝并返回 -1。

### T2 — `weaknet_config.{hpp,cpp}` + `edge_telemetry_exporter.{hpp,cpp}`

**配置代次**落盘在 `<state_path>.generation`，原子写
（`open → write → fsync → rename → fsync(dir)`，写循环带 EINTR 重试）。
CAS 成功才落盘，写失败只记日志、不阻断执行——配置代次是防重放的加强项，
不是可执行性前提，让它把一条已经成功的配置变更拖挂更糟。

**注意文件名刻意是 `.generation` 而不是复用 `state_path_`**：
`hasCrashRecoveryFile()` 的语义就是「`state_path_` 是否存在 = 是否有 TRIAL
崩溃残留」。把持久代次写进同一个文件会让一条正常的持久化把设备误判成
「上次升级崩在半路」。

**动作去重表**落盘在 `data_dir/executed_actions.jsonl`，一行 `action_id\tfirst_ok`，
`O_APPEND + fsync`，重复行允许（后者覆盖前者，读取时按序应用）。

- 存**结果**而不只是存在性：首次被拒的动作在回执丢失后被云端重放时，必须
  回执真实结果，不能谎报 APPLIED。
- 满 1024 条改为**FIFO 淘汰到半容量**（`executed_action_order_` 单独维护插入
  顺序，因为 `unordered_map` 的遍历顺序不稳定，「淘汰最旧」会退化成「随机淘汰」），
  淘汰后 `compactExecutedActions()` 重写文件。
- 构造函数里 `loadExecutedActions()` 而不是 `start()` 里：单测直接调
  `applyPendingActions()` 也要能看到磁盘状态。

### T3 — `looper.{hpp,cpp}` + `dbus_service.{hpp,cpp}` + `server.cpp`

**Looper 增加主线程任务队列**：`eventfd(EFD_NONBLOCK|EFD_CLOEXEC)` 唤醒 poll，
`postToMainThread()` 可从任意线程调用，`run()` 每轮 `drainWakeups()` 在 D-Bus
派发**之前**执行（任务本身就是「把算好的结果发出去」，早一轮发出让调用方少等
一个 poll 周期）。任务整体换出后再执行，避免链式入队自锁；单条任务抛异常不
中断其余任务。

**`Looper` 析构必须公开**：`std::unique_ptr<Looper>` 的默认删除器不是友元，
私有析构直接编译失败。这不泄漏构造入口——默认构造仍是私有的。

**DbusService 增加单工作线程 + 有界队列**（`kMaxSlowQueryQueue = 4`）：

| 位置 | 内容 |
|---|---|
| 派发线程 | 参数解析、limit 钳制、入队、Busy 回复 |
| worker | 四条只读查询的 SQLite 读取 + JSON 拼装 |
| 派发线程（回） | `dbus_message_new_method_return` + send + flush |

几条踩过的坑：

- **`Looper::current()` 必须在主线程捕获**（构造函数里存成 `looper_`）。
  它是 `thread_local` 懒构造，在 worker 里调用会凭空造出一个永不 `run()` 的
  实例，任务入队后无人消费——现象是「查询成功但客户端永远收不到回复」。
  这个坑没有任何报错，只有客户端超时。
- **读锁必须与指针解引用在 worker 内成对**。`QueryDeviceEvents` 与
  `QuerySiteIncidents` 要读 `ctx_->wireless_event_store`，而插件生命周期线程
  持写锁时会销毁它。主线程提前取指针再释放读锁，worker 手里的裸指针就重新
  暴露在 UAF 之下。
- **额度按「未答复总数」计，且在回复真正送出后才归还**。按「排队深度」计会
  让上限随 worker 取走动作漂移；在 worker 里 post 完就自减则会让排队中的
  8.5MB 结果一直堆在 Looper 任务队列里——背压彻底失效，正是这个上限要防的东西。
- **`serial == 0` 的消息无法被回复**：`dbus_message_new_method_return/error`
  对 `reply_serial == 0` 会**断言失败并 abort 整个进程**（不返回 NULL，无法用
  返回值兜住）。生产路径不可达（handler 收到的消息必然来自总线），但把
  「一条构造不合法的消息」升级成「服务 core dump」不划算，已显式丢弃并记日志。
  这是加单测时实测撞出来的。
- **worker 必须在共享资源释放前 join**。`ServerContext` 成员析构是声明逆序，
  `db_mgr`（第 149 行）先于 `service`（第 97 行）销毁，而 worker 的查询恰恰
  解引用 `db_mgr`。关停路径在 `Looper::run()` 返回后**立刻**
  `stopSlowQueryWorker()`；`~ServerContext` 里另有一道防御性收尾，覆盖
  「启动中途失败提前 return」这条不走正常关停段的路径。

### 验证

- x86：`ctest --test-dir build-x86/server` → **68/68 通过**（原 67 套 + 新增
  `test_slow_query_dbusservice_gtest`）。
- ARM64：`build-arm64-native` 交叉编译通过（仅既有 `-Wmismatched-tags` 告警）。
- 新增测试共 7 条，其中 Part B 走真实 `dbus-daemon`：
  - `DispatchThreadStaysResponsiveWhileQueryInFlight`：受控阻塞 job 占住 worker，
    断言 `ListInterfaces` 仍在 200ms 内答复（实测 254ms 含 200ms 轮询预算）。
  - `AsyncReplyDeliversSamePayloadAsDirectQuery`：真 SQLite 建库两行数据，
    断言异步路径的回复与直接 `queryHistory` **逐字节一致**，并断言额度已归还。
  - `QueueFullRepliesBusyImmediately`：队列满时客户端**立刻**拿到
    `com.example.WeakNet.Error.Busy`（实测 215ms，含总线往返）。

## 结果

三项全部落地，全量回归与 ARM64 交叉编译均通过。

### 修复的缺陷与可观测变化

| 缺陷 | 修复前表现 | 修复后 |
|---|---|---|
| 清理删掉存活事故的证据事件 | 跨越保留期的事故影响面静默变短，无任何错误 | 证据保留至事故本身过期 |
| 配置代次重启归零 | 重启后云端可重放已应用的配置 | 从 `.generation` 恢复 |
| 动作去重表重启清空 | OTA 被重复调度、TRIAL 看门狗被重置 | 从 `executed_actions.jsonl` 恢复 |
| 去重表满 1024 一次性 clear | 丢掉最旧条目（回执最可能丢失的那批） | FIFO 淘汰到半容量 |
| 只读查询占住派发线程 | 13h 窗口查询期间整个服务不响应 | 只有 worker 忙，派发线程照常答复 |

### 遗留

- **T4（AF_PACKET 主动探测帧）未实施，且不建议在本仓库实施**。
  它是 LoopGuard 里最有价值的部分，但它属于 L5 执行层，与产品定位
  「只观测、不控制」冲突。要用它，必须先改产品定位文档承认主动注入探测帧
  这一能力，并重新划定权责边界——这是产品决策，不是工程决策。
  经用户确认前**不得动工**。
- 去重表的淘汰策略是 FIFO。若将来出现「长期运行 + 高频动作下发」的场景，
  可能需要改成 LRU 或按时间窗淘汰，但当前数据不支持这个判断。
