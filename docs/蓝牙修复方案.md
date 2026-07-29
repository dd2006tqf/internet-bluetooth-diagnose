# 蓝牙功能闭环修复方案

## 一、问题概述

服务端蓝牙监测模块 (`bt_monitor.cpp`) 已完成数据采集与事件生成，但**事件路由存在错误**，导致客户端无法正确接收蓝牙事件。

---

## 二、关键断点

### 🔴 断点 1：蓝牙设备事件使用错误的信号名

**位置**：`bt_monitor.cpp:1063-1077`

**问题代码**：

```cpp
case BtEvent::Type::DeviceConnected:
case BtEvent::Type::DeviceDisconnected:
case BtEvent::Type::DeviceFound:
case BtEvent::Type::DeviceLost:
    // 蓝牙设备变化 → 发送通用信号
    if (ctx->service) {
        std::thread([ctx, ev]() {
            ctx->service->emitSpecificSignal(
                kSignalInterfaceChanged,  // ❌ 应该是 kSignalBluetoothDeviceChanged
                std::string("[BT] ") + ev.message, 0);
        }).detach();
    }
    break;
```

**后果**：
- 客户端通过 `subscribeToBluetoothEvents()` 订阅的是 `BluetoothDeviceChanged` 信号，但服务端实际发射的是 `InterfaceChanged` 信号 → **客户端收不到事件**
- `EventManager::emitBluetoothDeviceChanged()` 被调用后触发的 D-Bus 信号是 `BluetoothDeviceChanged`，但这部分代码从未被执行 → **EventManager 的蓝牙回调链永远不会触发**

---

### 🔴 断点 2：蓝牙 RSSI 混入 Wi-Fi RSSI 通道

**位置**：`bt_monitor.cpp:1079-1083`

**问题代码**：

```cpp
case BtEvent::Type::DeviceRssiChanged:
    getEventManager().emitRssiChanged(      // ❌ 应该用 emitBluetoothDeviceChanged()
        std::string("[BT] ") + ev.message, ev.deviceName);
    break;
```

**后果**：
- 蓝牙 RSSI 事件与 Wi-Fi RSSI 事件混在一起
- 订阅 Wi-Fi RSSI 的回调会收到蓝牙数据
- 蓝牙订阅收不到任何事件

---

### 🟡 次要问题

| 问题 | 位置 | 影响 |
|------|------|------|
| `DiscoveryStarted/Stopped` 事件静默丢弃 | `bt_monitor.cpp:1084` default 分支 | 低：扫描启停事件无外部通知 |
| `BluetoothDeviceChanged` 回调未注册 | `event_manager.cpp:151-165` | 中：即使修正事件路由，也没有默认回调 |

---

## 三、当前数据流（错误状态）

```
BlueZ D-Bus API
    │
    ✅ BtMonitor 数据采集
    │
    ✅ BtEvent 事件生成
    │
    ❌ 事件路由 (bt_monitor.cpp:1063-1083)
    │   ├── DeviceFound/Lost/Connected/Disconnected ──→ ❌ InterfaceChanged (错!)
    │   ├── DeviceRssiChanged ──→ ❌ RssiChanged (Wi-Fi 通道, 错!)
    │   └── DiscoveryStarted/Stopped ──→ ❌ 静默丢弃
    │
    ✅ EventManager (代码就绪但未使用)
    │
    ✅ D-Bus Service (GetBluetoothDevices/Adapter 查询就绪)
    │
    ✅ 客户端 (信号监听/API/订阅均已就绪)
```

---

## 四、修复方案

**修改文件**：仅需修改 `bt_monitor.cpp:1061-1089` 的事件路由 switch

### 修改前

```cpp
switch (ev.type) {
    case BtEvent::Type::DeviceConnected:
    case BtEvent::Type::DeviceDisconnected:
    case BtEvent::Type::DeviceFound:
    case BtEvent::Type::DeviceLost:
        // 蓝牙设备变化 → 发送通用信号
        if (ctx->service) {
            std::thread([ctx, ev]() {
                ctx->service->emitSpecificSignal(
                    kSignalInterfaceChanged,  // ❌ 错误
                    std::string("[BT] ") + ev.message, 0);
            }).detach();
        }
        break;

    case BtEvent::Type::DeviceRssiChanged:
        // RSSI 变化信号
        getEventManager().emitRssiChanged(  // ❌ 错误
            std::string("[BT] ") + ev.message, ev.deviceName);
        break;

    default:
        break;
}
```

### 修改后

```cpp
switch (ev.type) {
    case BtEvent::Type::DeviceConnected:
    case BtEvent::Type::DeviceDisconnected:
    case BtEvent::Type::DeviceFound:
    case BtEvent::Type::DeviceLost:
    case BtEvent::Type::DeviceRssiChanged:
    case BtEvent::Type::DiscoveryStarted:
    case BtEvent::Type::DiscoveryStopped:
        // 所有蓝牙事件统一走 EventManager，发射正确的信号
        getEventManager().emitBluetoothDeviceChanged(
            ev.message,
            ev.deviceName.empty() ? ev.deviceMac : ev.deviceName);
        break;

    case BtEvent::Type::AdapterAdded:
    case BtEvent::Type::AdapterRemoved:
    case BtEvent::Type::AdapterPowered:
        // 适配器事件直接发射 D-Bus 信号
        if (ctx->service) {
            ctx->service->emitSpecificSignal(
                kSignalBluetoothDeviceChanged, ev.message, 0);
        }
        break;
}
```

### 补充修改：注册蓝牙默认回调

**文件**：`event_manager.cpp:151-165`

在 `startEventMonitoring()` 中添加：

```cpp
registerCallback(EventType::BluetoothDeviceChanged, [](const NetworkEvent& event) {
    LOG_INFO(LogModule::EVENT_MGR, "Bluetooth device event: " << event.message);
});
```

---

## 五、修正后数据流

```
BtEvent ──→ EventManager::emitBluetoothDeviceChanged()
               │
               ├── bluetooth_callbacks_ (内部回调/日志)
               │
               └── emitSpecificSignal(kSignalBluetoothDeviceChanged, ...)
                      │
                      ▼
                   客户端 subscribeToBluetoothEvents() ✅ 完整闭环
```

---

## 六、验证清单

- [ ] 客户端调用 `subscribeToBluetoothEvents()` 后能收到 `DeviceFound` 事件
- [ ] 客户端调用 `subscribeToBluetoothEvents()` 后能收到 `DeviceLost` 事件
- [ ] 客户端调用 `subscribeToBluetoothEvents()` 后能收到 `DeviceConnected` 事件
- [ ] 客户端调用 `subscribeToBluetoothEvents()` 后能收到 `DeviceDisconnected` 事件
- [ ] 客户端调用 `subscribeToBluetoothEvents()` 后能收到 `DeviceRssiChanged` 事件
- [ ] 客户端调用 `subscribeToBluetoothEvents()` 后能收到 `DiscoveryStarted/Stopped` 事件
- [ ] 客户端调用 `weaknet_get_bluetooth_devices()` 能返回正确的设备列表
- [ ] 客户端调用 `weaknet_get_bluetooth_adapter()` 能返回正确的适配器信息
- [ ] Wi-Fi RSSI 事件与蓝牙 RSSI 事件不再混淆
- [ ] 服务端日志中出现 "Bluetooth device event:" 记录
