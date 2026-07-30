# Tasks: fix-wifi-iface-type-detection

- [x] 1 编写 Wi-Fi 接口类型识别单元测试、实现修复并完成全量回归与编译验证（RED + GREEN + REGRESSION）
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `Wi-Fi 接口类型识别应支持 cfg80211 驱动` | `接口存在 wireless 目录时应识别为 WiFi`
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `Wi-Fi 接口类型识别应支持 cfg80211 驱动` | `接口存在 phy80211 符号链接时应识别为 WiFi`
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `Wi-Fi 接口类型识别应支持 cfg80211 驱动` | `接口名以 wlan/wlp/wlx 开头且无 sysfs 标志时应识别为 WiFi`
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `Wi-Fi 接口类型识别应支持 cfg80211 驱动` | `有线接口不应被误识别为 WiFi`
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `RSSI 监控应覆盖所有被识别为 WiFi 的接口` | `cfg80211 Wi-Fi 接口应被 RSSI 监控覆盖`
  - Verify: `test` `build`
  - 新建 `server/test/unit/test_iface_type.cpp`，使用临时目录模拟 sysfs，覆盖 wireless 目录、phy80211 符号链接、接口名前缀、有线接口不误判等场景
  - 新建 `server/include/iface_type.hpp` 和 `server/src/iface_type.cpp`，实现 wireless 目录 + phy80211 符号链接 + 接口名前缀后备检查
  - 修改 `server/src/weak_netmgr.cpp`：移除 static `isWirelessInterface`，改为 `#include "iface_type.hpp"`
  - 修改 `server/Makefile`：将 `iface_type.cpp` 加入 SRC 列表，添加 test_iface_type 编译规则
  - RED 阶段：单元测试失败（phy80211 和前缀后备未实现）
  - GREEN 阶段：实现修复，单元测试全部通过
  - REGRESSION 阶段：运行 `test-all` 全量回归（test kind）与 `build-server` 编译验证（build kind），确认 weak_netmgr.cpp 调用路径 collectCurrentInterfaces → isWirelessInterface → setType(WiFi) → updateWifiRssi 不跳过 WiFi 接口
