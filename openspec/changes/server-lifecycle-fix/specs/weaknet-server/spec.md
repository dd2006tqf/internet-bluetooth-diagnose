# lifecycle-fix Specification

## ADDED Requirements

### Requirement: 服务端生命周期统一管理

服务端启动与退出时 **MUST** 统一管理所有监控线程与资源生命周期，保证进程在退出过程中线程已被收回、资源已被释放，避免野指针访问与资源泄漏。

#### Scenario: 所有监控线程可被主线程 join

- **WHEN** 服务端开始退出
- **THEN** MUST 在返回主入口前 join 全部捕获 `ServerContext` 的监控线程（含蓝牙/RTT/Jitter/RSSI），且 join 时序必须在释放上下文资源之前

#### Scenario: 停止顺序先停循环再释放

- **WHEN** 服务端退出
- **THEN** MUST 先将运行标志置为 false 使各线程退出循环，再 release 各监控器与 D-Bus 连接等资源，避免工作线程在资源被销毁后仍访问

#### Scenario: ServerContext 析构释放资源

- **WHEN** `ServerContext` 生命周期结束
- **THEN** MUST 释放其持有的 D-Bus 连接、WeakNetMgr 与 D-Bus 服务对象

#### Scenario: 接口列表唯一事实源

- **WHEN** 服务端运行期间并发读写接口列表
- **THEN** MUST 统一通过 `WeakNetMgr` 维护唯一的接口列表并在 D-Bus 服务读取时经其线程安全接口访问，避免上下文内多个事实源与无锁写竞争

### Requirement: D-Bus 信号发射线程安全

服务端在多个监控线程向同一个 D-Bus 连接发射信号时，**MUST** 保证发射操作线程安全，且不通过脱离管理的 detached 线程访问服务上下文。

#### Scenario: 信号发射加互斥

- **WHEN** 多个监控线程并发调用 D-Bus 服务发射信号
- **THEN** MUST 使用互斥保护连接写入，且发射调用在所属监控线程内同步完成，不创建 detached 子线程
