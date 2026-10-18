# 蓝牙事件指针生命周期锁纪律修复（C1/C5/C6）

> 日期：2026-10-18
> 分类：快速修复
> 结果：已完成

## 构思

全仓 code review（`/full-code-review`）发现三个相互关联的缺陷：

- **C1（Critical）**：`BtEventsPlugin` 发布到 `ServerContext` 的三个跨插件指针
  （`wireless_event_store` / `bt_link_quality_tracker` / `site_incident_correlator`）
  在启动发布、停止析构时完全不持锁，而 `bt_hci_traffic` worker、`BtMonitor`
  主循环与 D-Bus 查询分发均在其它线程并发解引用这些指针，插件停止（如
  `weaknet-cli monitor restart bt_events`）期间存在确定性 UAF 窗口。
  其余 12 个插件早已遵循「发布/销毁持 `unique_lock`、消费持 `shared_lock` 判空后使用」
  的既有纪律，唯独 bt_events 三个指针游离在外。
- **C5**：`dbus_service.cpp` 头注释声称这些指针"仅在构造期初始化一次"，与
  运行期可被插件启停改写的现实不符，误导后续维护。
- **C6**：`UsingInterfaceManager` 单例的 Netlink 事件线程只在静态析构里被隐式
  收尾，没有显式 `stop()`；关机序列无法保证该线程在 `ShutdownGoogleLogging()`
  之前完全终止。

曾尝试以 OpenSpec change `fix-bt-events-pointer-lifecycle` 走完整工作流
（Task 1~3 为锁改造/接口收口，Task 4 为并发生命周期回归套件）。Task 1~3
以 `observability_only` 例外的 ALTERNATIVE 证据完成；但 Task 4 是**纯测试新增、
被测行为已在 Task 1~3 实现完毕**的任务，严格 RED→GREEN→REGRESSION 在 harness
契约下机械不可行：RED 要求测试失败且测试文件指纹冻结，GREEN/REGRESSION 要求
同 argv 全绿——一个永久失败的守卫断言无法转绿，去掉守卫又无法产生 RED。
中途改过一次例外划分（abort 重冻结）仍未解开 Task 4 的互斥，按「遇阻塞必问」
向用户呈报后，用户决策整体废弃 change、转为工作流外修复。

## 实现方式

- `server/src/monitor_ebpf_plugins.cpp`
  - `BtEventsPlugin::start`：三指针发布包进 `std::unique_lock<std::shared_mutex>`；
  - `BtEventsPlugin::stop`：独占锁内原子完成解绑关联器 → 置空上下文指针 →
    析构三成员，读者要么看到完整对象、要么看到 `nullptr` 安全降级；
  - `BtHciTrafficPlugin`：新增 `dependencies() == {"bt_events"}`，使
    `MonitorManager` 启停感知依赖拓扑；worker 注入 `LinkDegraded` 事件改为
    `shared_lock` 内判空 + `recordEvent`。
- `server/include/bt_monitor.hpp` + `server/src/bt_monitor.cpp`：废除构造期
  一次性缓存的 `tracker_` / `eventStore_` 裸指针，改为持 `ServerContext*`，
  周期刷新（射频新鲜观测、轮询快照）与每 6 轮基线持久化路径全部
  `shared_lock` 现取现用。
- `server/src/dbus_service.cpp`：`QueryDeviceEvents` / `QuerySiteIncidents`
  处理函数在共享锁内判空查询；头注释改为"运行期受 monitor_pointers_mutex
  保护"的真实描述（C5）。
- `server/include/using_iface.h` + `server/src/using_iface.cpp`：新增幂等
  `stop()`（置 `running=false`，`join()` 在 `stateMutex_` 锁外执行避免与
  `publishState` 死锁）；析构函数复用 `stop()`（C6）。
- `server/src/server.cpp`：关机序列在 `Logger::stopFileLog()` /
  `ShutdownGoogleLogging()` 之前显式调用 `UsingInterfaceManager::stop()`。
- `server/test/unit/test_bt_events_lifecycle_gtest.cpp`（新增）+
  `server/test/CMakeLists.txt`：并发生命周期回归套件——空指针共享锁安全读、
  写线程 start/stop 循环 vs 4 读线程 300ms 压测无 UAF、
  `UsingInterfaceManager::stop()` 三连调幂等。

验证：x86 全量 `cmake --build build-x86 && ctest --test-dir build-x86/server`
52/52 套件全部通过（含新增 `test_bt_events_lifecycle_gtest`）。

## 结果

已完成。三个缺陷（C1/C5/C6）全部修复，回归套件并入常规 ctest 集。
遗留：代码尚在工作区未提交（含 `ai_snapshot.json` 的 change-aborted 状态），
需用户授权后随评审修复批次一并 commit。无升格为 OpenSpec change 的必要——
行为语义未变，仅补齐既有锁纪律与停机接口。
