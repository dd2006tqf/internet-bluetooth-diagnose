# Proposal: server-lifecycle-fix

## Why

当前服务端存在一批同一根因的并发/生命周期缺陷：监控线程与信号线程以裸指针捕获 `ServerContext`（`start_server` 的**栈对象**），且多数线程 detach、无所有权、无 join，shutdown 时无法保证它们在 ctx 存活期内安全退出。具体表现为：

1. **蓝牙线程 detached 且不被 join**（`bt_monitor.cpp`）——其可能在 shutdown 后仍访问已析构的 ctx。
2. **多个监控线程 detached，成员句柄未被赋值**（`rtt/jitter/rssi/bt` 内部 `.detach()`；`rtt_thread` 成员从未赋值，无 jitter/rssi/bt 成员）——"统一 join"实际不生效，shutdown 后这些线程仍可能访问悬垂 ctx。
3. **裸指针资源从不释放**——`WeakNetMgr*`/`DbusService*`/DBus 连接在正常 shutdown 均未释放；`weak_mgr` 在多线程里无锁 `new` 存在数据竞争。
4. **信号发送线程全部 detach + 无 D-Bus 发射锁**——shutdown 后访问悬垂 `ctx->service`，并与 D-Bus 主循环并发发信号。
5. **停止顺序错误**——主线程先 `stop()` 监控器再置 `running=false`/join 工作线程，导致 worker 可能在资源被销毁后仍调用 `getStats()`。

## What

建立统一的"线程所有权 + 资源所有权"模型，保证主线程在返回前绝对能：**全部捕获 `ctx*` 的线程已 join、全部资源已释放、DBus 连接已 release**。

- 补齐 `ServerContext` 线程句柄（jitter/rssi/bt），`rtt_thread` 改为实际赋值；各线程由 detached 改为 joinable。
- 修正退出顺序：先 `running=false` → join 全部线程 → 再 release 监控器与 DBus 连接等资源。
- `WeakNetMgr`/`DbusService`/DBus 连接改为受上下文析构管理的 RAII 资源。
- D-Bus 信号发射加互斥，移除所有 detached 信号子线程，改由所属监控线程同步发射。
- `weak_mgr` 改为启动期一次性创建，消除多线程并发 `new`。
- 接口列表收口为唯一事实源（`WeakNetMgr::current_interfaces_`），D-Bus 服务统一经其线程安全接口读取。

## 非目标 / 边界

- **不改 eBPF 取数逻辑**（DNS 响应键不匹配、HTTP kretprobe 寄存器误用）——另开变更。
- 不改 spec 需求语义、不引入后端队列/发布订阅重构。
- 不改 `wifi_packet_loss.bpf.c`（已单独提交，不在本变更文件清单内）。

## 影响

- 涉及：`server/include/server.hpp`、`server/src/server.cpp`、各监控器 `start_*_thread`（rtt/jitter/rssi/bt/tcp_loss）、`dbus_service.hpp/.cpp`、`init_dbus`、接口列表读写。
- 行为上不改变采集逻辑，仅修正线程收尾与资源释放时序；对进程正常退出的确定性有显著提升。
