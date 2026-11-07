# hid-multitouch 外部模块：不重编内核补回多点触控能力

> 日期：2026-11-07
> 分类：工作流外实施（承接 `驱动面能力引入与业务融合方案.md` §7.5 路线 C）
> 结果：已完成（驱动能力已具备并实测；业务未接入，等产品定位修订）

## 构思

### 要解决的问题

方案 §7.5 选定「HDMI 显示器 + USB 电容触控板（¥30-90）」作为最便宜的触摸路线。
但这条路线撞上了两个互相矛盾的流传说法，都必须在动手前用实测判掉：

| 说法 | 来源 | 实测结论 |
| --- | --- | --- |
| 「USB 触摸屏免驱、内核自带」 | 网上教程、用户转来的方案 | **在本板不成立** |
| 「所以要支持触摸必须重编内核」 | 上一版方案的推断 | **同样不成立** |

**第一条为什么不成立。** 板端内核 `5.15.147-99btf1-a733` 的 HID 相关配置：

```
CONFIG_HID=y                          内建
CONFIG_HID_GENERIC=y                  内建
CONFIG_USB_HID=y                      内建
CONFIG_INPUT_TOUCHSCREEN=y            内建
# CONFIG_HID_MULTITOUCH is not set         ← 没有 MT 协议解析器
# CONFIG_TOUCHSCREEN_USB_COMPOSITE is not set
# CONFIG_TOUCHSCREEN_EGALAX is not set
# CONFIG_TOUCHSCREEN_ELAN is not set
# CONFIG_HID_ELAN is not set
```

`modprobe hid_multitouch` / `modprobe usbtouchscreen` 均返回
`FATAL: Module ... not found in directory /lib/modules/5.15.147-99btf1-a733`。

后果是**假绿灯**：`lsusb` 有、`/dev/input/event*` 有、`evtest` 也能打印坐标，
看起来一切正常。但那只是内建 `hid-generic` 给的**单点鼠标语义**
（`ABS_X` / `ABS_Y` / `BTN_LEFT`）。真正的多点触控要求 `ABS_MT_SLOT`(0x2f)
与 `ABS_MT_TRACKING_ID`(0x39)，没有 `hid-multitouch` 就永远拿不到。

**第二条为什么也不成立。** 重编内核会同时打碎三件东西：

| 若重编内核 | 后果 |
| --- | --- |
| eBPF `vmlinux.h` | BTF 变化 → 预置头失效 |
| kmod vermagic | `weaknet_cpu_stat` 正在加载，vermagic 一变全部 `Unknown symbol` / 拒绝加载 |
| BSP `sunxi-drm` | 厂商补丁需重新适配，显示栈归零 |

而这三条的风险前提全部是「替换内核」。**做外部模块不碰内核二进制**，三条全部落空。

### 为什么不新开 OpenSpec change

用户已明确「除非明确要求，否则不走 OpenSpec change」，该偏好已写入项目 memory
`no-new-openspec-change-by-default`。本次是能力补全（新增一个不在发布链路上的
外部模块），不改变任何既有行为，按 `openspec/tasks/README.md` 记录即可。

### 备选方案与取舍

| 方案 | 成本 | 判定 |
| --- | --- | --- |
| **外部模块搬运上游 `hid-multitouch.c`** | 零硬件，一次性 | ✅ 选定 |
| 重编内核开 `CONFIG_HID_MULTITOUCH=y` | 数小时 + 上述三项风险 | ❌ 代价与收益严重失衡 |
| 官方 MIPI DSI 屏（I2C GT911，内核 `goodix.ko` 已带） | ¥250-350 | ❌ 太贵，且是「设备树工程」不是本项目要练的东西 |
| 放弃多点，只当单点鼠标用 | ¥0 | ❌ 那样触摸对 `qt_dashboard` 毫无价值 |

## 实现方式

### 一、源头：原样搬运，零本地修改

| 文件 | 来源 | 行数 |
| --- | --- | --- |
| `kmod/hid_multitouch/hid-multitouch.c` | `torvalds/linux` **v5.15** `drivers/hid/hid-multitouch.c` | 2201 |
| `kmod/hid_multitouch/hid-ids.h` | `torvalds/linux` **v5.15** `drivers/hid/hid-ids.h` | 1356 |

取 v5.15 是因为板端内核是 5.15 系列，`vermagic` 与内部 API 才能对齐。
许可证 `GPL-2.0-or-later`（模块内 `MODULE_LICENSE("GPL")`）。

**不做任何本地修改。** 这不是「我们自己的驱动」，是原样搬运的上游代码。
需要改行为时应先向上游确认，而不是在这里打补丁——一旦打了补丁，
下次同步上游就要重新做一遍 rebase，且没人记得改过什么。

### 二、前置条件（动手前逐条实测）

外部模块能不能用，只取决于这四条，与「内核有没有编这个驱动」无关：

| 前提 | 实测值 |
| --- | --- |
| 有内核头文件 | `/lib/modules/5.15.147-99btf1-a733/build` 存在 |
| 无签名强制 | `# CONFIG_MODULE_SIG is not set` |
| vermagic 一致 | 编译产物与 `uname -r` **逐字相同** |
| 符号已导出 | 38 个未定义符号**全部**在 `Module.symvers` |
| 板端工具链 | gcc 10.2.1-6 / make 4.3 / ld 2.35.2 / bc 1.07.1（**不需要 bison/flex**，那是全量内核构建才要） |

一个容易误判的点：`nm` 里查不到 `hid_register_driver` **不是符号缺失**。
它在 5.15 是 `hid.h:884-885` 的宏，展开为 `__hid_register_driver`，后者已导出。

### 三、能力自证：`tools/probe_virtual_mt.py`

这一步是整个任务里最关键的**证据设计**。

`insmod` 成功只证明**驱动注册**成功——它不证明能绑定设备，更不证明产出的是
MT 语义而非又一份单点鼠标。没有实物触控板时，很容易在这里糊弄过去。

做法是用 `/dev/uhid` 造一个 Digitizer/Touch Screen 设备，让内核 HID 核心判为
`HID_GROUP_MULTITOUCH`，命中 `hid-multitouch` `id_table` 末尾的通用兜底条目：

```c
/* Generic MT device */
{ HID_DEVICE(HID_BUS_ANY, HID_GROUP_MULTITOUCH, HID_ANY_ID, HID_ANY_ID) },
```

然后检查产出的 input 节点能力位，并写一帧触摸报告确认事件贯通。

退出码分层，便于自动化判定：`0` 能力闭环 / `2` 未绑定 / `3` 缺 MT 能力位 /
`4` 未收到 MT 事件 / `5` 环境不满足。

**这个脚本证明了什么、没证明什么**（边界必须写清，否则会被当作「触控板已验证」）：

- **证明**：报告描述符解析、MT 字段映射、input 槽位分配。这三件事与设备是
  USB 还是 uhid 无关。
- **未证明**：具体触控板的固件行为、VID/PID 是否需要 `new_id`、贴合精度。

因此结论只能是「**驱动能力已具备**」，不能是「触控板已验证可用」。

### 四、定位：不进发布包（刻意）

`kmod/Makefile` 里现有的三个模块是**无害的观测类**（只读内核数据），
加载与否不改变系统行为。`hid-multitouch` 不同：它会与内建 `hid-generic`
**争抢同一批 USB HID 设备**——一旦默认加载，现场所有 HID 设备
（键鼠、条码枪、工业手柄）的绑定关系都会改变。

所以它**不并入 `kmod/Makefile`、不被 `tools/ci.sh` 收集、不进 OTA manifest**，
需要时由人显式 `make && insmod`。

### 五、验证手段

| 层 | 手段 | 结果 |
| --- | --- | --- |
| 板端编译 | `make` | rc=0，`hid-multitouch.ko` 572960 字节 |
| 模块属性 | `strings \| grep -E '^(vermagic\|license\|description\|depends)='` | `license=GPL`、`depends=`（空）、vermagic 逐字一致 |
| 符号依赖 | `readelf -sW ... \| awk '$7=="UND"'` 逐条对 `Module.symvers` | 38 个全中，一个不缺 |
| 加载 | `insmod` | rc=0，`/sys/bus/hid/drivers/hid-multitouch` 出现（含 `bind`/`unbind`/`new_id`），与内建 `hid-generic` 并存 |
| 设备绑定 | `probe_virtual_mt.py` | `driver=hid-multitouch`，dmesg `input: weaknet-vts ...` |
| MT 能力位 | 同上 | `ABS_MT_SLOT` / `POSITION_X` / `POSITION_Y` / `TRACKING_ID` **全部 YES** |
| 事件贯通 | 同上 | 一帧报告 → 7 个 input 事件，含 `ABS_MT_TRACKING_ID=0` + `BTN_TOUCH=1` + `EV_SYN` |
| 卸载 | `rmmod` | rc=0，目录干净消失，`hid-generic` 不受影响 |

## 结果

### 已达成的

- 驱动能力**已具备**：板端编译、vermagic 匹配、38 符号全导出、加载/卸载可逆、
  虚拟 MT 设备绑定成功、MT 能力位齐全、单帧触摸事件贯通。
- 证伪了「必须重编内核」这条推断，内核二进制一动没动，eBPF / kmod / BSP 零风险。
- 证伪了「USB 触摸屏免驱」这条流传说法，并给出了正确的验证方法
  （看 `capabilities/abs` 的两个位，而不是 `lsusb` / `evtest` / `evdev` 三连）。

### 边界（诚实记录）

1. **真实触控板未验证。** 虚拟设备只证明驱动与内核 HID 栈对接正确，
   不证明具体硬件的固件行为、VID/PID 是否要 `new_id`、贴合精度。
2. **多点并发手势未验证。** 只写了一帧报告，没有做双指、滑动、捏合。
3. **与 `qt_dashboard` 的手势接线未做。** 仓库内 28 个 `assurance/` 头文件零触摸引用，
   `MetricId` 17 项全是网络类，`qt_dashboard` 只有一个「刷新」按钮
   （`main_window.cpp:125`，`&QPushButton::clicked`），无 `mousePressEvent` /
   `touchEvent` / `QGesture`。
4. **不进发布包，默认不加载。** 这是刻意设计而非未完成项，理由见「实现方式 · 四」。

### 与 LRADC 的区别

方案 §13 的 LRADC 按键驱动同样「能跑通但不接入」，但两者性质不同：

| | LRADC 按键 | hid-multitouch |
| --- | --- | --- |
| 判定 | **永久判定为练习** | **能力已备好，临时搁置** |
| 何时接入 | 不会 | 产品定位改完后即可接线，不必回头补驱动 |

### 后续

接入的前置条件不是驱动，而是**产品定位与消费者**。用户已决定（2026-11-07）
后续修改产品定位，给触摸一个真实消费者；届时：

1. 买 USB 电容触控板（¥30-90）；
2. `make && sudo insmod hid-multitouch.ko`；
3. 若落到 `hid-generic`，用 `new_id` 或 `unbind`/`bind` 补救（`README.md` 有模板）；
4. 在 `qt_dashboard` 里加手势处理，并在 `MetricId` 侧考虑是否需要输入类指标。

**不建议升格为 OpenSpec change。** 本模块不进发布链路、不改变任何既有行为、
无生产消费者，不满足「行为变更」前提。真要纳入受管流程，应在有消费者且
有明确行为规格（触摸事件如何影响诊断输出）时再做，而不是现在追认。
