# 杂项安全性与稳健性修固（C3/C4/S5/S9）

> 日期：2026-10-19
> 分类：快速修复
> 结果：已完成

## 构思

全仓 code review 提出的 4 个中小严重度稳定性与安全隐患：

- **C3（Minor）**：`WeakNetMgr::updateWifiRssiSafe` 过去在 `iface_mutex_` 独占锁内
  直接调用包含阻塞式 Unix Domain Socket/netlink I/O 的 `updateWifiRssi`，
  持锁时间长达毫秒级，造成高频读网卡的其它线程（D-Bus dispatch/looper）被锁。
- **C4（Minor）**：`NetworkEpochStore::writeState` 原子 rename 临时文件后，
  未对父目录执行 `fsync(dirfd)`；掉电时目录项（dentry）若未刷盘，重启可能
  观测到老文件名或全零。
- **S5（Minor）**：`tools/ci.sh` 过去通过 `ssh ... sed -i ... "token: ${EDGE_TOKEN}" /tmp/weaknet-config.yaml`
  将预共享令牌以命令行实参（argv）形式传递（存在 `ps` 观测窗口）且暂存
  于 `/tmp` 世界可读文件。
- **S9（Minor）**：`ActionRegistry::safeExec` 子进程过去未设置独立进程组，
  超时时仅对主 PID 发送 `kill(pid, SIGKILL)`，产生的孤儿子孙进程可能逃逸残留。

## 实现方式

- `server/src/weak_netmgr.cpp`（C3）
  - 复制 `rtt_monitor`/`jitter` 的「锁内拷贝快照 → 锁外 I/O → 短锁回写」模式；
  - 步骤 1：短锁拷贝 `current_interfaces_` 中所有 WiFi 网卡名；
  - 步骤 2：锁外执行 `connect` / `readNl80211Rssi` / `readProcWirelessRssi` 等 I/O；
  - 步骤 3：短锁仅遍历更新数值与 `snapshot_generation_`；
  - 步骤 4：锁外向 `metrics_registry_` 发布。
- `server/src/network_epoch_store.cpp`（C4）
  - `rename` 成功后，提取父目录路径并以 `O_RDONLY | O_DIRECTORY` 打开；
  - 对 `dirfd` 调用 `::fsync()` 并关闭，确保目录项元数据刷盘。
- `tools/ci.sh`（S5）
  - 本地渲染待部署配置为 0600 临时文件并以 `shred -u` 安全销毁；
  - 通过 SSH stdin 管道直接由 `sudo install -m 0600 /dev/stdin /etc/weaknet/config.yaml`
    写入受保护路径，令牌绝不进入 argv、亦不落地 `/tmp`。
- `server/include/assurance/action_registry.hpp`（S9）
  - `posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP)` +
    `posix_spawnattr_setpgroup(&attr, 0)` 将排查命令放入独立进程组；
  - 超时强杀由 `kill(pid, SIGKILL)` 修正为 `kill(-pid, SIGKILL)` 杀整组。

验证：x86 `ctest` 52/52 套件通过（含 `test_weak_netmgr_gtest` 与
`test_network_epoch_store_gtest`）。
