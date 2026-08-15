# Tasks: server-lifecycle-fix

- [ ] 1 补齐 ServerContext 线程句柄（jitter/rssi/bt）并使 rtt_thread 实际赋值
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `服务端生命周期统一管理` | `所有监控线程可被主线程 join`
  - Verify: `build`
  - rtt/jitter/rssi/bt 的 start_*_thread 由 detached 改为写入 ctx->xxx_thread（含新增句柄）

- [ ] 2 ServerContext 资源 RAII（WeakNetMgr/DbusService/DBus 连接）并由析构释放
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `服务端生命周期统一管理` | `ServerContext 析构释放资源`
  - Verify: `build`
  - weak_mgr/service 改 unique_ptr（或受析构 delete），~ServerContext 释放 DBus 连接

- [ ] 3 修正退出顺序：running=false → join 全部 → 释放（移除双 stop）
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `服务端生命周期统一管理` | `停止顺序先停循环再释放`
  - Verify: `build`
  - 重排 start_server() 退出路径，移除显式 monitor->stop()

- [ ] 4 D-Bus 发射加互斥并移除 detached 信号线程（同步发射）
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `D-Bus 信号发射线程安全` | `信号发射加互斥`
  - Verify: `build`
  - DbusService emit* 加锁；各 detach 信号子线程改为同步调用

- [ ] 5 消除 weak_mgr 多线程并发创建（启动期一次性创建）
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `服务端生命周期统一管理` | `接口列表唯一事实源`
  - Verify: `build`
  - 删除各监控线程内 `if (!ctx->weak_mgr) new`，由 start_server 预建

- [ ] 6 接口列表唯一事实源收口（dbus 改读 WeakNetMgr，修 ctx->iface_list 无锁写）
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `D-Bus 信号发射线程安全` | `信号发射加互斥`
  - Verify: `build`
  - dbus_service 改用 ctx->weak_mgr->getCurrentInterfaces()；移除/加锁 ctx->iface_list 写
