# hid-multitouch 外部模块

多点触控 HID 驱动，作为**外部模块**构建，不随发布包分发。

## 为什么需要它

板端内核（`5.15.147-99btf1-a733`）的 HID 相关配置是：

| 配置项 | 状态 | 后果 |
|---|---|---|
| `CONFIG_HID=y` | 内建 | HID 核心可用 |
| `CONFIG_HID_GENERIC=y` | 内建 | 任意 USB HID 设备都会「有反应」 |
| `CONFIG_USB_HID=y` | 内建 | USB HID 传输层可用 |
| `CONFIG_INPUT_TOUCHSCREEN=y` | 内建 | input 子系统支持触摸屏 |
| `CONFIG_HID_MULTITOUCH` | **未设置** | **没有 MT 协议解析器** |
| `CONFIG_TOUCHSCREEN_USB_COMPOSITE` | **未设置** | 没�� USB 触摸屏复合驱动 |

「内核自带驱动、免驱」这句话在这里**不成立**。`hid-generic` 会接管任意 USB HID 设备，
包括触控板——所以 `lsusb` 有、`/dev/input/event*` 有、`evtest` 也能打印坐标。
但那只是**单点鼠标语义**：`ABS_X` / `ABS_Y` / `BTN_LEFT`。
真正的多点触控要求 `ABS_MT_SLOT` / `ABS_MT_TRACKING_ID`，这必须有 `hid-multitouch`。

这就是本模块存在的唯一理由。

## 上游来源

| 文件 | 来源 | 说明 |
|---|---|---|
| `hid-multitouch.c` | `torvalds/linux` **v5.15** `drivers/hid/hid-multitouch.c` | 2201 行，未改动 |
| `hid-ids.h` | `torvalds/linux` **v5.15** `drivers/hid/hid-ids.h` | 1356 行，未改动（`hid-multitouch.c` 的 `#include` 依赖） |

取自 v5.15 是因为板端内核是 5.15 系列，`vermagic` 与内部 API 才能对齐。
许可证：`GPL-2.0-or-later`（模块内 `MODULE_LICENSE("GPL")`）。

**不做任何本地修改。** 这不是「我们自己的驱动」，是原样搬运的上游代码。
需要改行为时应先向上游确认，而不是在这里打补丁。

## 前置条件（已在本板实测成立）

| 前提 | 实测值 | 为什么重要 |
|---|---|---|
| 有内核头文件 | `/lib/modules/5.15.147-99btf1-a733/build` 存在 | 没有就编不了外部模块 |
| 无签名强制 | `# CONFIG_MODULE_SIG is not set` | 否则 `insmod` 直接被拒 |
| vermagic 一致 | `5.15.147-99btf1-a733 SMP preempt mod_unload aarch64`，与内核逐字相同 | 不一致内核拒绝加载 |
| 符号已导出 | 38 个未定义符号**全部**在内核 `Module.symvers` 中 | 缺一个就 `Unknown symbol` |

注意 `hid_register_driver` 在 5.15 是**宏**（`hid.h:884-885`），展开为 `__hid_register_driver`。
`nm` 里看不到 `hid_register_driver` 是正常的，不代表符号缺失。

## 构建

必须在**板端**编译。宿主机交叉编译也能出 `.ko`，但内核若开启了
`CONFIG_MODVERSIONS` 或 BTF 差异会导致 `insmod` 拒绝；板端编译是唯一受支持方式。

```bash
cd kmod/hid_multitouch
make                    # 产出 hid-multitouch.ko
make clean              # 清理中间产物
```

实测产物：`hid-multitouch.ko`，572960 字节，
`license=GPL`、`depends=`（空）、`vermagic` 与内核一致。

板端 `modinfo` 不可用（kmod 工具未装）。替代查证：

```bash
strings hid-multitouch.ko | grep -E '^(vermagic|license|description|depends)='
readelf -sW hid-multitouch.ko | awk '$7=="UND"{print $8}'   # 未定义符号
```

## 加载 / 卸载

```bash
sudo insmod hid-multitouch.ko
sudo rmmod hid_multitouch      # 注意：模块名是下划线，不是连字符
```

实测：`insmod` rc=0，`/sys/bus/hid/drivers/` 下出现 `hid-multitouch` 目录
（含 `bind` / `unbind` / `new_id` / `uevent` / `module`），与内建的 `hid-generic` 并存；
`rmmod` rc=0，目录干净消失，`hid-generic` 不受影响。

**`hid-generic` 不需要也不能 `rmmod`**——它是内建的（`=y`），不在 `lsmod` 里。
两者会同时存在于 `/sys/bus/hid/drivers/`，具体设备由 HID 核心按 group 与 id_table 择优绑定。

## 能力自证（无物理触控板时）

`insmod` 成功只证明**驱动注册**成功，不证明能绑定设备，也不证明产出的是 MT 语义。
`tools/probe_virtual_mt.py` 补上这两环：

```bash
sudo python3 tools/probe_virtual_mt.py
```

它用 `/dev/uhid` 造一个 Digitizer/Touch Screen 设备，内核 HID 核心判为
`HID_GROUP_MULTITOUCH`，命中 `hid-multitouch` id_table 末尾的通用条目：

```c
/* Generic MT device */
{ HID_DEVICE(HID_BUS_ANY, HID_GROUP_MULTITOUCH, HID_ANY_ID, HID_ANY_ID) },
```

然后检查产出的 input 节点能力位，并写一帧触摸报告确认事件贯通。

**实测输出（2026-11-07，板端）：**

```
HID 设备目录: 0003:222A:0001.0006
绑定驱动: hid-multitouch
✓ 已由 hid-multitouch 绑定

input 节点: /dev/input/event11
  ABS_MT_SLOT            YES
  ABS_MT_POSITION_X      YES
  ABS_MT_POSITION_Y      YES
  ABS_MT_TRACKING_ID     YES
✓ MT 能力位齐全

收到 7 个 input 事件:
  EV_ABS ABS_MT_TRACKING_ID = 0
  EV_ABS ABS_MT_POSITION_X  = 1000
  EV_ABS ABS_MT_POSITION_Y  = 2000
  EV_KEY BTN_TOUCH = 1
  EV_ABS ABS_X = 1000
  EV_ABS ABS_Y = 2000
  EV_SYN
✓ 事件从 HID 层贯穿到 input 层
=== EXIT: 0 ===
```

退出码含义：`0` 能力闭环 / `2` 未绑定 / `3` 缺 MT 能力位 / `4` 未收到 MT 事件 / `5` 环境不满足。

**这个脚本证明了什么、没证明什么：**

- 证明：驱动与内核 HID 栈对接正确——报告描述符解析、MT 字段映射、input 槽位分配。
  这三件事与设备是 USB 还是 uhid 无关。
- 未证明：具体触控板的固件行为、VID/PID 是否需要 `new_id`、贴合精度。
- **因此不宣称「触控板已验证可用」**，只宣称「驱动能力已具备」。

## 实物接入时的注意事项

### 1. VID/PID 可能不在表里

`id_table` 以固定 VID/PID 条目为主，通用兜底条目要求设备被内核判为
`HID_GROUP_MULTITOUCH`。部分廉价触控板描述符不规范，可能落到 `hid-generic`。
查证：

```bash
# 插上触控板后
cat /sys/bus/hid/devices/*/modalias
ls /sys/bus/hid/devices/*/driver -l          # 看绑给了谁
```

若绑给了 `hid-generic`，两条路：

```bash
# 路 A：先试 new_id 强制匹配
sudo sh -c 'echo "0003 <VID> <PID>" > /sys/bus/hid/drivers/hid-multitouch/new_id'

# 路 B：把 hid-generic 与新设备解绑，让 hid-multitouch 抢
sudo sh -c 'echo "<hiddev>" > /sys/bus/hid/drivers/hid-generic/unbind'
sudo sh -c 'echo "<hiddev>" > /sys/bus/hid/drivers/hid-multitouch/bind'
```

`new_id` 只写入动态匹配表；总线上没有该 VID/PID 设备时不会有任何可见效果（rc 仍为 0），
这是预期行为，不是失败。

### 2. `new_id` 的持久化

`new_id` 只对当前启动有效，reboot 后丢失。需要持久化就写 udev 规则：

```
# /etc/udev/rules.d/99-hid-multitouch.rules
ACTION=="add", SUBSYSTEM=="hid", ATTRS{idVendor}=="<vid>", ATTRS{idProduct}=="<pid>", \
  RUN+="/bin/sh -c 'echo $kernel > /sys/bus/hid/drivers/hid-generic/unbind; \
                    echo $kernel > /sys/bus/hid/drivers/hid-multitouch/bind'"
```

### 3. 验证触控板是否真是多点触控

用户侧的 `lsusb` / `ls /dev/input/event*` / `evtest` 打印坐标，**三步全过也不能证明多点触控**。
必须看能力位：

```bash
sudo apt install -y evtest
sudo evtest /dev/input/eventX      # 两指同时按下，看是否出现 ABS_MT_SLOT 切换
# 或者直接查 sysfs：
cat /sys/class/input/eventX/device/capabilities/abs
```

`ABS_MT_SLOT`(0x2f) 与 `ABS_MT_TRACKING_ID`(0x39) 两个位都为 1 才是真多点。
只有 `ABS_X`/`ABS_Y` 的是单点鼠标语义。

## 定位：不进发布包

**本模块不并入 `kmod/Makefile`，不被 `tools/ci.sh` 收集，不进 OTA manifest，不随发布包分发。**

原因：`kmod/Makefile` 里的三个模块是**无害的观测类模块**（只读内核数据）。
而 `hid-multitouch` 会与内建 `hid-generic` 争抢同一批 USB HID 设备，
**改变系统输入行为**——一旦默认加载，所有 HID 设备（键鼠、条码枪、工业手柄）
的绑定关系都会变。这类模块必须由人显式加载。

所以它是「能力已就绪、默认不启用」。真正需要时：

```bash
cd /path/to/kmod/hid_multitouch && make && sudo insmod hid-multitouch.ko
```

## 边界声明

- 已验证：板端编译、vermagic 匹配、38 符号全导出、`insmod`/`rmmod` 可逆、
  虚拟 MT 设备绑定、MT 能力位齐全、单帧触摸事件贯通。
- 未验证：真实触控板的兼容性、多点并发手势、与 `qt_dashboard` 的手势接线。
- **触摸缺少业务消费者**：仓库内 28 个 assurance 头文件零触摸引用，
  `MetricId` 17 项全是网络类。模块可用 ≠ 项目需要它。接入与否取决于产品定位。
