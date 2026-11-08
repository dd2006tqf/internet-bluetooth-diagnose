# hid-multitouch 外部模块

多点触控 HID 驱动，作为**外部模块**构建。**随发布包分发，但默认不加载**——
插上多点触控设备时由 udev 规则按需 `insmod`（见「发布与加载模型」一节）。

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

**宿主机交叉编译与板端编译都成立**（2026-11-08 实测订正）。

> **本文档此处此前写的是「板端编译是唯一受支持方式」，该结论已被实测推翻。**
> 当时的推理是「内核若开启 `CONFIG_MODVERSIONS` 或 BTF 差异会导致 `insmod` 拒绝」，
> 但**没有实际查过本板是否开启**。实测结果：
>
> | 检查 | 实测值 | 含义 |
> | --- | --- | --- |
> | `CONFIG_MODVERSIONS` | **not set** | 无符号 CRC 校验，不看编译器版本 |
> | `CONFIG_MODULE_SIG` / `_FORCE` | **not set** | 不强制签名 |
> | `CONFIG_DEBUG_INFO_BTF` | `=y` | 但 headers 无 vmlinux，模块 BTF 生成被跳过；内核加载不要求模块带 BTF |
>
> 决定性验证：在 x86 宿主机用 gcc 13.3.0 交叉编译（内核是 gcc 10.2.1 编的），
> 产物 vermagic 逐字匹配，板端 `insmod` **rc=0**、`lsmod` 可见、`rmmod` 可逆。
> 编译器版本差异在 `MODVERSIONS` 关闭时不影响加载。

```bash
# 推荐：宿主机交叉编译（随发布包走，不需要板上装内核头文件）
./tools/kmod_cross_build.sh          # 三个模块一起，含本模块

# 板端就地编译（仅用于调试验证）
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

## 发布与加载模型（2026-11-08 决策：打包但不自动加载）

**本模块随发布包分发，但默认不加载。** 插上多点触控设备时由 udev 规则按需加载。

| | `weaknet-kmod.service`（观测模块） | 本模块（输入模块） |
| --- | --- | --- |
| 加载时机 | 开机无条件加载 | **插上 MT 设备才加载** |
| 加载器 | systemd oneshot | udev 规则 `99-weaknet-hid-multitouch.rules` |
| 副作用 | 无（只读内核数据） | **改变 HID 绑定行为** |
| 失败后果 | 观测面 unavailable | 触摸退化成单点鼠标 |

为什么这个区别必须存在：`hid-multitouch` 一旦在内核里，HID 核心会把**所有**
被判为 `HID_GROUP_MULTITOUCH` 的设备优先绑给它而不给内建的 `hid-generic`。
对触摸屏这是改善（拿到 `ABS_MT_SLOT` / `ABS_MT_TRACKING_ID` 多点语义），
但它是**全局输入行为变更**——一台不带触摸屏的网关没有任何理由承担它。
所以开关由「设备是否存在」这个物理事实驱动，而不是由开机顺序驱动。

### udev 规则匹配条件

HID 设备的 `modalias` 形如 `hid:b0003g0002v0000222Ap00000003`，
其中 group 字段就是内核判定的 HID group（`include/linux/hid.h:370-373`）：

| group | 常量 | 是否加载 |
| --- | --- | --- |
| `g0001` | `HID_GROUP_GENERIC` | ❌ 键盘、鼠标、条码枪 |
| `g0002` | `HID_GROUP_MULTITOUCH` | ✅ 标准多点触控 |
| `g0003` | `HID_GROUP_SENSOR_HUB` | ❌ |
| `g0004` | `HID_GROUP_MULTITOUCH_WIN_8` | ✅ Win8 兼容多点触控 |

**双向实测（2026-11-08 板端）：**

| 场景 | 结果 |
| --- | --- |
| 虚拟 MT 设备（`g0002`）插入，模块未加载 | ✅ udev 自动 `insmod`，随后 MT 能力位齐全、7 个 input 事件贯通 |
| 虚拟标准鼠标（`g0001`）插入，模块未加载 | ✅ 模块**未**被加载，鼠标仍由 `hid-generic` 绑定 |

### 加载顺序无关性（实测）

设备**先**被 `hid-generic` 绑定，**之后**才 `insmod hid-multitouch`，
内核会自动把该设备改绑到 `hid-multitouch`。**无需**任何手动
`bind` / `unbind` / `new_id`。所以规则即使在 `hid-generic` 已接管之后才生效，
也能把设备抢回来。

### 手工启用 / 停用

```bash
# 手工加载（规则已覆盖常见路径，通常不需要）
sudo insmod /home/radxa/weaknet/current/kmod/hid-multitouch.ko

# 停用：卸载模块并把设备还给 hid-generic（卸载即自动回退）
sudo rmmod hid_multitouch

# 看设备绑给了谁
ls -l /sys/bus/hid/devices/*/driver
```

## 边界声明

- 已验证：宿主机交叉编译产物可被板端加载（`MODVERSIONS` 关闭）、vermagic 匹配、
  38 符号全导出、`insmod`/`rmmod` 可逆、虚拟 MT 设备绑定、MT 能力位齐全、
  单帧触摸事件贯通、**udev 按需加载（正反双向）**。
- 未验证：真实触控板的兼容性、多点并发手势、与 `qt_dashboard` 的手势接线。

## 消费者状态（2026-11-08 更新）

| 阶段 | 状态 |
| --- | --- |
| 驱动能力 | ✅ 已就绪（本模块） |
| 产品消费者 | ✅ **已定义**：现场交互终端白名单的三项（趋势指标切换 / 时间窗切换 / 截图导出）。边界见 `docs/产品定位-工业无线诊断网关.md` 第三节之二 |
| UI 消费者 | ✅ **已落地**：`qt_dashboard` 的指标下拉框 + 三个时间窗按钮 + 导出按钮（commit `ee4507e`） |
| 发布链 | ✅ **已接入**：`tools/kmod_cross_build.sh` 交叉编译、`tools/ci.sh` 分发并安装 udev 规则 |
| 默认加载 | ⬜ **刻意不做**：改为 udev 按需加载（见上节），这是设计决定而非欠账 |
| 真实触摸屏 | ⏳ 待硬件：驱动链路已闭环，物理设备未到位 |

> **历史沿革**：本 README 曾两次因前提变化而失效——第一次是「触摸缺少业务消费者」
> （定位 v1.2 承认现场终端后作废），第二次是「板端编译是唯一受支持方式」
> 与「不进发布包」（2026-11-08 实测 `MODVERSIONS` 关闭后一并推翻）。
> 两次的共同教训：**没有实测支撑的保守结论，会变成不必要的阻塞**。
