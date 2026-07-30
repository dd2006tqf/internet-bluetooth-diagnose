# Spec Delta: fix-wifi-iface-type-detection

## ADDED Requirements

### Requirement: Wi-Fi 接口类型识别应支持 cfg80211 驱动

接口类型识别逻辑 MUST 通过多种 sysfs 标志判断接口是否为 Wi-Fi，包括旧的 WEXT 标志（`/sys/class/net/<iface>/wireless` 目录）和新的 cfg80211 标志（`/sys/class/net/<iface>/phy80211` 符号链接）。当 sysfs 标志不存在时，SHALL 使用接口名前缀（wlan/wlp/wlx）作为后备判断。

#### Scenario: 接口存在 wireless 目录时应识别为 WiFi

- **Given** 系统中存在网络接口 `wlan0`
- **When** `/sys/class/net/wlan0/wireless` 目录存在
- **Then** 该接口应被识别为 `NetType::WiFi`

#### Scenario: 接口存在 phy80211 符号链接时应识别为 WiFi

- **Given** 系统中存在网络接口 `wlan0`，且 `/sys/class/net/wlan0/wireless` 目录不存在
- **When** `/sys/class/net/wlan0/phy80211` 符号链接存在
- **Then** 该接口应被识别为 `NetType::WiFi`

#### Scenario: 接口名以 wlan/wlp/wlx 开头且无 sysfs 标志时应识别为 WiFi

- **Given** 系统中存在网络接口 `wlan0`，且 `/sys/class/net/wlan0/wireless` 和 `/sys/class/net/wlan0/phy80211` 均不存在
- **When** 接口名以 `wlan`、`wlp` 或 `wlx` 前缀开头
- **Then** 该接口应被识别为 `NetType::WiFi`（接口名前缀后备）

#### Scenario: 有线接口不应被误识别为 WiFi

- **Given** 系统中存在网络接口 `eth0`
- **When** `/sys/class/net/eth0/wireless` 和 `/sys/class/net/eth0/phy80211` 均不存在
- **Then** 该接口不应被识别为 `NetType::WiFi`

### Requirement: RSSI 监控应覆盖所有被识别为 WiFi 的接口

RSSI 监控线程（`updateWifiRssi`）MUST 对所有 `NetType::WiFi` 类型的接口尝试获取 RSSI，不跳过因 sysfs 标志缺失而曾经被误判的接口。

#### Scenario: cfg80211 Wi-Fi 接口应被 RSSI 监控覆盖

- **Given** 系统中存在 Wi-Fi 接口 `wlan0`，仅具有 `phy80211` 符号链接
- **When** RSSI 监控线程执行 `updateWifiRssi`
- **Then** 该接口的 `type` 应为 `NetType::WiFi`，不应被跳过
