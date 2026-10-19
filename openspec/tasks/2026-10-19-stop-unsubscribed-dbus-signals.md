# O3-B：停发 7 个零订阅者 D-Bus 信号

> 日期：2026-10-19
> 分类：工作流外实施（full-code-review 修复批次 2）
> 结果：已完成

## 构思

评审发现 7 个 D-Bus 信号（`Changed`/`InterfaceChanged`/`ConnectionModeChanged`/
`NetworkQualityChanged`/`BluetoothDeviceChanged`/`MonitorStateChanged`/
`EbpfMonitorStats`）生产端全在发射、**零生产订阅者**：`server.cpp` 注释宣称
「信号替代轮询」的替代消费者从未建出来，客户端订阅 API 只有示例/演示程序在用。
二选一：A. 建 `weaknet-cli stats --watch` 类订阅消费者；B. 停发。用户选 **B**。

停发范围决策（沿用评审时点结论）：
- **服务端发射全停 + 死管线删除**：不止"不发"，发射方法本身（emitChanged/
  emitSpecificSignal/ emitNetworkQualitySignal / sendSignalInternal /
  send_mutex_ / Changed 载荷落盘 / EventManager 内信号分发段）零调用后
  即孤儿接口，按铁律一并删除；
- **客户端订阅 API 暂留**（weaknet_subscribe_* / weaknet_check_* /
  deserializeChangedPayloadFromFile）：删除是另一块表面，留待后续批次
  统一处置，文档已标注"订阅后永远等不到事件"。

测试耦合是本批的前置阻塞：mock 计数桩（test_recorder）+ 两个测试断言的是
「D-Bus 发射路径活着」，必须先解耦再删发射，顺序不能反。

## 实现方式

**测试解耦（先做）**：
- `test_event_manager_gtest.cpp`：删 Test8 `EventCounterAtomicUnderConcurrency`
  （被测对象 eventCounter 只服务信号载荷，信号没了测试空转；注释留档原因）；
  fixture 不再构造 ServerContext/DbusService，`startEventMonitoring()` 无参化；
  移除 mock 链接。
- `test_bt_full_link_gtest.cpp`：保留 feed→detect→emit→callback 全链断言，
  删 signal-sink 计数断言（Test1 尾、Test2 尾）与 ctx_/svc_ fixture；移除 mock 链接。
- `mock_dbus_service.cpp`：删 emit 桩 + test_recorder，只剩 ServerContext 析构桩
  （server.cpp 不进 server_lib，test_bt_events_lifecycle 仍需它链接）。
- `server/test/CMakeLists.txt`：前两个 target 去掉 mock 源。

**发射删除（后做）**：
- `event_manager.cpp/hpp`：emitEvent 只剩 invokeCallbacks；删 getSignalName、
  server_ctx_；startEventMonitoring 改无参（保留其注册默认日志回调的职责）；
  头/文件注释同步（孵化中 EventType 的说明改写）。
- `dbus_service.cpp/hpp`：删 4 个发射方法 + sendSignalInternal + send_mutex_
  + ChangedPayload 落盘；文件头「方法处理+信号发射」改「方法处理」；补 `<mutex>`
  （cpp:993 conflict_mutex 仍需）。
- `server.cpp`：删 3 处 emitChanged（接口/流量/上网方式线程，保留
  EventManager 回调分发）；删 emitEbpfStats + appendEvictionJson 定义与 5 处
  调用（dns/http/process/tcp_retrans/tcp_conn 的"细粒度信号" JSON 块）；
  MonitorStateChanged 回调改为日志（状态变化可观测性不丢）；启动日志去掉
  「信号 Changed」；change_counter 删除。
- `rssi/rtt/tcp_loss_monitor.cpp`：删 emitChanged 条件块，顺带清掉只为信号
  存在的 rtt_changed/jitter_changed/updated 死变量。
- `bt_monitor.cpp`：适配器事件（AdapterAdded/Removed/Powered）原直接发信号，
  停发后该 case 只留注释——设备级事件仍走 EventManager 回调。
- **不变的能力**：keyStatsSnapshot 逐 key 驱逐可见性仍经
  `GetEbpfMonitorHealth`（dbus_service.cpp:~1535）对外暴露，未随信号死亡。

**文档同步**（文档与代码一致性硬规则）：
- `docs/架构设计-模块与代码结构.md`：架构图信号行、目录注释、"主动推送的
  信号"整节改为停发说明 + 出口对照表、EventManager 职责、客户端订阅/轮询
  两节标注失效、流程 2/3/4 图重画、D-Bus 设计决策理由修正、eviction 示例
  归位到查询方法语境。
- `docs/学习路线图.md`：监控线程表信号列改「原 D-Bus 信号（O3-B 后已停发）」。
- `docs/蓝牙监控优化实现方案.md`：AC1.5 验收方法从 dbus-monitor 改为
  test_bt_full_link 回调断言。

## 结果

- x86 全量 ctest **53/53 通过**（server 52 + client test_lifecycle_api）。
- 仓库内 tools/scripts/板端测试脚本无任何信号消费者（grep 零命中），
  停发不影响部署脚本。
- 遗留：客户端订阅 API（weaknet_subscribe_*/weaknet_check_*）与
  serializeChangedPayloadToFile 已无服务端写入方——待后续批次删除或重建
  发射（须先有真实订阅者）；`kSignal*` 常量因客户端仍在引用暂留。
- 后续批次待办不变：O4/O7/O8/O9 清理、O11-O16 云侧消费者、S4/S6/S9、
  gitleaks、CMake GLOB、板端统一验证。
