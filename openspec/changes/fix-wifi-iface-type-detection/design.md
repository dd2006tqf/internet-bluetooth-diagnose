# Design: fix-wifi-iface-type-detection

## Investigation

### 当前实现

`server/src/weak_netmgr.cpp` 第 17-21 行定义了 static 函数 `isWirelessInterface`：

```cpp
static bool isWirelessInterface(const std::string& ifname) {
    std::string path = "/sys/class/net/" + ifname + "/wireless";
    struct stat st;
    return (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode));
}
```

该函数仅检查 WEXT（Wireless Extensions）的 `/sys/class/net/<iface>/wireless` 目录。

`collectCurrentInterfaces()` 第 32-38 行使用该函数设置接口类型：

```cpp
if (isWirelessInterface(n)) {
    info.setType(NetType::WiFi);
} else if (n.rfind("eth", 0) == 0 || n.rfind("enp", 0) == 0) {
    info.setType(NetType::Ethernet);
} else {
    info.setType(NetType::Unknown);
}
```

`updateWifiRssi()` 第 92-94 行根据类型跳过非 Wi-Fi 接口：

```cpp
if (x.type() != NetType::WiFi) {
    LOG_INFO(LogModule::WEAK_MGR, "updateWifiRssi: skipping non-WiFi interface " << x.ifName());
    continue;
}
```

### 根因

Radxa Cubie A7A 开发板的 Wi-Fi 驱动基于 cfg80211，不创建 WEXT 兼容的 `wireless` 目录，仅创建 `phy80211` 符号链接。因此 `isWirelessInterface("wlan0")` 返回 false，接口被设置为 `NetType::Unknown`，RSSI 监控跳过该接口。

### 消费者分析

- `rssi_monitor.cpp:37` — 检查 `net.type() == NetType::WiFi` 决定是否输出 RSSI 日志
- `weak_netmgr.cpp:92` — `updateWifiRssi` 检查 `x.type() != NetType::WiFi` 决定是否跳过
- `network_quality_assessor.cpp` — 使用 `rssiDbm()` 进行质量评分（RSSI=-1000 导致评分错误）

## Alternatives considered

### 方案 A：仅修改 isWirelessInterface 增加 phy80211 检查（已选定）

在现有 `isWirelessInterface` 函数中增加 `phy80211` 符号链接检查和接口名前缀后备。将函数提取到可测试的辅助文件中以支持单元测试。

优点：修改范围最小，直接解决根因。
缺点：需要提取函数到新文件以支持测试。

### 方案 B：使用 nl80211 netlink 接口查询

通过 nl80211 netlink 协议查询接口是否为 Wi-Fi。

优点：最权威的判断方式。
缺点：实现复杂度高，需要新增 netlink 代码，超出最小修复范围。

### 方案 C：使用 ioctl SIOCGIWNAME

通过 WEXT ioctl 查询接口是否支持无线扩展。

优点：不需要 sysfs。
缺点：cfg80211 驱动可能不实现 WEXT ioctl，与当前问题相同。

## Chosen approach: 方案 A

### 修改文件

1. **新建 `server/include/iface_type.hpp`** — 声明 `isWirelessInterface` 函数，接受可选 sysfs base 路径参数（默认 `/sys/class/net`），便于单元测试
2. **新建 `server/src/iface_type.cpp`** — 实现 `isWirelessInterface` 函数，检查 `wireless` 目录 + `phy80211` 符号链接 + 接口名前缀后备
3. **修改 `server/src/weak_netmgr.cpp`** — 移除 static `isWirelessInterface`，改为 include `iface_type.hpp`
4. **修改 `server/Makefile`** — 将 `iface_type.cpp` 加入编译列表
5. **新建 `server/test/unit/test_iface_type.cpp`** — 单元测试，使用临时目录模拟 sysfs

### 识别逻辑

```cpp
bool isWirelessInterface(const std::string& ifname, const std::string& sysfsBase) {
    // 1. WEXT 标志：/sys/class/net/<iface>/wireless 目录
    std::string wirelessPath = sysfsBase + "/" + ifname + "/wireless";
    struct stat st;
    if (stat(wirelessPath.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
        return true;
    }
    // 2. cfg80211 标志：/sys/class/net/<iface>/phy80211 符号链接/目录
    std::string phyPath = sysfsBase + "/" + ifname + "/phy80211";
    if (stat(phyPath.c_str(), &st) == 0) {
        return true;
    }
    // 3. 接口名前缀后备（wlan/wlp/wlx）
    if (ifname.rfind("wlan", 0) == 0 ||
        ifname.rfind("wlp", 0) == 0 ||
        ifname.rfind("wlx", 0) == 0) {
        return true;
    }
    return false;
}
```

## TDD Policy

本变更为行为变更，适用 TDD（RED → GREEN → REFACTOR → REGRESSION）。

- RED：先编写单元测试，覆盖 wireless 目录、phy80211 符号链接、接口名前缀、有线接口不误判等场景，测试应失败（因为当前函数不支持 phy80211 和前缀后备）
- GREEN：实现修复，使测试通过
- REFACTOR：提取函数到独立文件，确保 weak_netmgr.cpp 调用正确
- REGRESSION：运行全量测试确保无回归

<!-- autoai:tdd-policy:v1 -->
```json
{
  "schema_version": 1,
  "default": "required",
  "exceptions": []
}
```
<!-- /autoai:tdd-policy:v1 -->

## Reuse candidates

- `server/test/unit/test_common.hpp` — 复用现有测试宏和 glog 初始化
- `server/Makefile` — 复用现有编译规则，仅追加一个源文件

<!-- autoai:implementation-economy:v2 -->
```json
{
  "schema_version": 2,
  "profile": "small",
  "rationale": "修复 Wi-Fi 接口类型识别缺陷：提取 isWirelessInterface 到独立可测试文件，增加 phy80211 和接口名前缀识别。变更范围限于一个新源文件、一个新头文件、一个新测试文件，以及对 weak_netmgr.cpp 和 Makefile 的小幅修改。",
  "classification": {
    "production": ["server/src/iface_type.cpp", "server/include/iface_type.hpp", "server/src/weak_netmgr.cpp"],
    "tests": ["server/test/unit/test_iface_type.cpp"],
    "project_docs": ["openspec/changes/fix-wifi-iface-type-detection/**"],
    "project_tooling": ["server/Makefile"],
    "examples": [],
    "generated": [],
    "vendor": []
  },
  "thresholds": {
    "production": {
      "added_lines": {"expected": 60, "review_at": 100, "hard_limit": 150},
      "touched_files": {"expected": 3, "review_at": 5, "hard_limit": 7},
      "new_files": {"expected": 2, "review_at": 3, "hard_limit": 4}
    },
    "tests": {
      "added_lines": {"expected": 120, "review_at": 200, "hard_limit": 300},
      "touched_files": {"expected": 1, "review_at": 2, "hard_limit": 3},
      "new_files": {"expected": 1, "review_at": 2, "hard_limit": 3}
    },
    "project_support": {
      "added_lines": {"expected": 5, "review_at": 15, "hard_limit": 30},
      "new_files": {"expected": 0, "review_at": 1, "hard_limit": 2}
    },
    "generated": {
      "files": {"expected": 0, "review_at": 0, "hard_limit": 0},
      "bytes": {"expected": 0, "review_at": 0, "hard_limit": 0}
    }
  },
  "structural_allowances": {
    "public_contracts": [{"id": "contract-001", "name": "isWirelessInterface", "reason": "新增内部辅助函数声明，供 weak_netmgr.cpp 调用和单元测试"}],
    "build_targets": [],
    "build_graph_entries": [{"id": "build-001", "name": "Makefile iface_type.cpp", "reason": "将 iface_type.cpp 加入 SRC 列表和 test_iface_type 编译规则"}],
    "distribution_surfaces": [],
    "direct_dependencies": []
  },
  "reuse_decisions": [
    {"id": "reuse-001", "path": "server/src/weak_netmgr.cpp", "symbol": "isWirelessInterface", "decision": "extend", "reason": "扩展现有 static 函数为可测试的独立函数，增加 phy80211 和前缀后备识别"}
  ],
  "obsolete_items": [],
  "exceptions": []
}
```
<!-- /autoai:implementation-economy:v2 -->

## Risks

- **兼容性**：增加 `phy80211` 检查不会影响已有 WEXT 驱动的识别（WEXT 检查仍优先执行）
- **回滚**：如需回滚，恢复 `weak_netmgr.cpp` 中的 static 函数并删除新增文件即可
- **依赖**：无新增外部依赖，仅使用标准 POSIX `stat()`

## Integration Completeness v1

<!-- autoai:integration-completeness:v1 -->
```json
{
  "schema_version": 1,
  "discovery": {
    "mode": "reviewed_inventory",
    "compile_commands_path": null
  },
  "surfaces": [
    {
      "id": "surface-iface-type-detection",
      "kind": "internal_api",
      "name": "isWirelessInterface(ifname, sysfsBase)",
      "change_kind": "added",
      "contract_impact": "compatible",
      "compatibility": null,
      "producer_paths": ["server/include/iface_type.hpp", "server/src/iface_type.cpp"],
      "consumer_kind": "production_caller",
      "consumer_paths": ["server/src/weak_netmgr.cpp"],
      "entrypoint": "WeakNetMgr::collectCurrentInterfaces sets NetType::WiFi",
      "evidence_contracts": [
        {
          "probe_id": "probe-iface-type-test-current",
          "kind": "test",
          "role": "current",
          "argv": ["scripts/project_command.sh", "test-all", "--change", "fix-wifi-iface-type-detection", "--json"],
          "expected_exit_codes": [0],
          "output_contains": "PASS"
        },
        {
          "probe_id": "probe-iface-type-build-current",
          "kind": "build",
          "role": "current",
          "argv": ["scripts/project_command.sh", "build-server", "--change", "fix-wifi-iface-type-detection", "--json"],
          "expected_exit_codes": [0],
          "output_contains": "weaknet-dbus-server"
        }
      ],
      "requirement_refs": [
        {
          "spec_path": "specs/weaknet-server/spec.md",
          "operation": "ADDED",
          "requirement": "Wi-Fi 接口类型识别应支持 cfg80211 驱动",
          "scenarios": ["接口存在 wireless 目录时应识别为 WiFi"]
        },
        {
          "spec_path": "specs/weaknet-server/spec.md",
          "operation": "ADDED",
          "requirement": "Wi-Fi 接口类型识别应支持 cfg80211 驱动",
          "scenarios": ["接口存在 phy80211 符号链接时应识别为 WiFi"]
        },
        {
          "spec_path": "specs/weaknet-server/spec.md",
          "operation": "ADDED",
          "requirement": "Wi-Fi 接口类型识别应支持 cfg80211 驱动",
          "scenarios": ["接口名以 wlan/wlp/wlx 开头且无 sysfs 标志时应识别为 WiFi"]
        },
        {
          "spec_path": "specs/weaknet-server/spec.md",
          "operation": "ADDED",
          "requirement": "Wi-Fi 接口类型识别应支持 cfg80211 驱动",
          "scenarios": ["有线接口不应被误识别为 WiFi"]
        },
        {
          "spec_path": "specs/weaknet-server/spec.md",
          "operation": "ADDED",
          "requirement": "RSSI 监控应覆盖所有被识别为 WiFi 的接口",
          "scenarios": ["cfg80211 Wi-Fi 接口应被 RSSI 监控覆盖"]
        }
      ],
      "task_ids": ["1"],
      "verify_kinds": ["test", "build"],
      "task_obligations": [
        {
          "task_id": "1",
          "verify_kinds": ["test", "build"],
          "evidence_roles": ["current"]
        }
      ],
      "expected_observation": "Wi-Fi 接口 wlan0 被识别为 NetType::WiFi，RSSI 监控线程不再跳过该接口，网络质量评分使用真实 RSSI 值",
      "symbol_identities": null
    }
  ]
}
```
<!-- /autoai:integration-completeness:v1 -->
