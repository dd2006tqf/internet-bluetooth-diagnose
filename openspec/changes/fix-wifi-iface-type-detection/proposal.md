# Proposal: fix-wifi-iface-type-detection

## Goals

- 修复 Wi-Fi 接口类型识别缺陷，使基于 cfg80211 的现代 Wi-Fi 驱动（不创建 `/sys/class/net/<iface>/wireless`）能被正确识别为 `NetType::WiFi`
- 恢复 RSSI 监控线程对 Wi-Fi 接口的 RSSI 采集能力
- 恢复网络质量评估器对 Wi-Fi 信号的准确评分

## Non-goals

- 不修改 wpa_supplicant 控制套接字通信逻辑（`net_wifiriss.cpp`）
- 不修改网络质量评估算法（`network_quality_assessor.cpp`）
- 不修改 eBPF、蓝牙或其他监控线程
- 不修改 D-Bus 接口和信号定义

## Problem

开发板（Radxa Cubie A7A）上 `wlan0` 已正常连接 Wi-Fi，但程序日志显示：

```text
processing interface wlan0 type=0
skipping non-WiFi interface wlan0
```

导致 `RSSI: -1000dBm`，网络质量评分错误为 `POOR`。

根因在 `server/src/weak_netmgr.cpp` 的 `isWirelessInterface()` 函数（第 17-21 行）：仅检查 `/sys/class/net/<iface>/wireless` 目录。基于 cfg80211 的现代 Wi-Fi 驱动不创建该 WEXT 目录，而是创建 `/sys/class/net/<iface>/phy80211` 符号链接。因此 `wlan0` 被误判为非 Wi-Fi 接口，`setType(NetType::Unknown)`，RSSI 监控线程跳过该接口。

## Affected capabilities

- 接口类型识别（`WeakNetMgr::collectCurrentInterfaces`）
- Wi-Fi RSSI 采集（`WeakNetMgr::updateWifiRssi`）
- 网络质量评估（`NetworkQualityAssessor`，因 RSSI 输入错误）

## Likely code areas

- `server/src/weak_netmgr.cpp` — `isWirelessInterface()` 函数（核心修复点）
- `server/src/weak_netmgr.cpp` — `collectCurrentInterfaces()` 接口类型设置逻辑
- `server/test/unit/` — 新增接口类型识别单元测试

## External contract impact

None. 本次变更不修改任何公共 API、D-Bus 接口、信号定义或数据结构。仅修改内部实现逻辑，使接口类型识别更准确。
