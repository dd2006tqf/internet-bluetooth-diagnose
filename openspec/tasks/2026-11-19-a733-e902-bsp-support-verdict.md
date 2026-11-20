# A733 E902 协处理器板级支持结论：sun60iw2 无板级定义

> 日期：2026-11-19
> 分类：硬件能力结论（只读核查，未改任何生产代码/配置）
> 结论：**路线 1（E902）在 sun60iw2 上不可行；路线 2（A55 隔离）是唯一自主可达路径**

## 构思

**要回答什么**：A733 的玄铁 E902 RISC-V 协处理器能不能用来做跨核通信？

**为什么现在要下定论**：此前一轮排查得出「E902 确认存活」，随后被 FIFO 下溢
假象推翻（见 `openspec/tasks/2026-11-18-a733-msgbox-path-verification.md`）。
但「A 核侧全静默」只能证明**现在没在用**，不能证明**将来没法用**。
中间又出现一份声称「Tina 5.0 BSP 有完整 rproc 支持」的材料，
必须用板上真实源码验证它。

**为什么走 `openspec/tasks/` 而不是 OpenSpec change**：这是只读取证 + 结论，
不改产品代码，零 footprint。

**取证取舍**：

| 手段 | 取舍 |
| --- | --- |
| 板端 `/usr/src/linux-headers-*/bsp/include/` | ✅ **决定性**。这是 Radxa 分发内核时带上的厂商 BSP 头文件 |
| `/proc/config.gz` | ✅ 运行内核实际配置 |
| 板端 DT（`/proc/device-tree`） | ✅ 运行时真实生效的硬件描述 |
| `/lib/firmware`、`/boot` 搜固件 | ✅ 确认是否有可加载的 E902 固件 |
| 全志 Tina SDK | ❌ **手上没有**，只能从 Radxa 分发的部分推断 |

## 实现方式

### 核查 1：先修正上一轮的假否定

**上一轮的错误**：用 `grep -r e902 /usr/src/linux-a733-5.15/` 得到 0 命中，
据此说「全树无 e902」。

**真相**：该目录**顶层 0 个条目**——是个空目录：

```
$ sudo ls -1 /usr/src/linux-a733-5.15/ | wc -l
0
```

真正的内核树是 `linux-headers-5.15.147-100btf1amp-a733`，含
`arch/ bsp/ include/ Makefile Module.symvers scripts/` 六个条目，
其中 `bsp/include/` 是厂商 BSP 头文件。

已单独成篇：`openspec/pitfalls/empty-dir-grep-false-negative.md`。

### 核查 2：真实 BSP 头文件里有什么

```
[remoteproc] 命中 2 个文件
  bsp/include/linux/rpbuf.h
  bsp/include/remoteproc/sunxi_remoteproc.h
[rpmsg] 命中 1 个文件
  bsp/include/linux/aw_rpmsg.h
[rpbuf] 命中 4 个文件
```

**但看内容，两个「存在」的头文件都是空壳：**

`sunxi_remoteproc.h` **全文 33 行**，全部实质内容只有一个资源类型常量和一个结构体：

```c
#define RSC_AW_TRACE 129
struct fw_rsc_aw_trace {
    u32 da;
    u32 len;
    u32 reserved;
    u8 name[32];
} __packed;
```

外加三个 `#if IS_ENABLED(CONFIG_SUNXI_RPROC_SHARE_IRQ)` 保护的函数声明。
**没有 rproc_id 枚举、没有 SRAM 布局、没有固件格式定义、没有 E902 字样。**

`aw_rpmsg.h` **全文 26 行**，只有：

```c
#ifdef CONFIG_AW_RPMSG_NOTIFY
int rpmsg_notify_add(const char *ser_name, const char *name, notify_callback cb, void *dev);
int rpmsg_notify_del(const char *ser_name, const char *name);
#endif
#ifdef CONFIG_AW_RPROC_FAST_BOOT
#define fast_rpmsg_driver(__rpmsg_driver) ...
#endif
```

**没有任何 E902/E906/E907 的通道号、endpoint 定义。**

**关键：整棵树没有任何 remoteproc 驱动源码（`.c`），只有头文件。**

### 核查 3：e902 的时钟/复位定义在哪颗芯片上

全树搜 `e902` 只有 2 个文件命中，**都是 `sun65iw1`**：

```
bsp/include/dt-bindings/reset/sun65iw1-r-ccu.h:20:  #define RST_BUS_E902_CFG   10
bsp/include/dt-bindings/clock/sun65iw1-r-ccu.h:30:  #define CLK_E902_24M       20
bsp/include/dt-bindings/clock/sun65iw1-r-ccu.h:31:  #define CLK_E902_CFG       21
bsp/include/dt-bindings/clock/sun65iw1-r-ccu.h:32:  #define CLK_E902_CORE      22
```

**`sun60iw2`（本板）的时钟头文件里没有任何 E902 定义。**

对照命名差异：

| 芯片 | R-CCU 里的 RISC-V 相关定义 |
| --- | --- |
| `sun65iw1` | `CLK_E902_24M` / `E902_CFG` / `E902_CORE` / `RST_BUS_E902_CFG` |
| `sun60iw2` | 只有通用名 `CLK_RISCV_24M` / `CLK_RISCV_CFG` / `CLK_RISCV`，**无 E902 字样** |

### 核查 4：arch 与 DT 源

```
arch/ 下搜 e902 命中: 0
```

**运行中的 DT 里也没有**协处理器节点，`reserved-memory` 只有一块 `bl31`
（`0x48000000`，16MB），**没有给任何协处理器预留内存**。

对照 `sun60iw1p1`：有 `e906_rproc@7130000` + 两块 `0x80000` 的
`riscvsram0/1_reserved` + 四个 msgbox bank。**sun60iw2 一个都没有。**

### 核查 5：运行内核配置

```
CONFIG_MAILBOX=y                        ← 传输层有
CONFIG_SUN6I_MSGBOX=y                   ← msgbox 驱动有
CONFIG_AW_MSGBOX=y                      ← 驱动已绑定 3004000.msgbox
# CONFIG_REMOTEPROC is not set          ← 生命周期管理没有
# CONFIG_RPMSG_VIRTIO is not set        ← 通信层没有
# CONFIG_AW_REMOTEPROC is not set       ← 厂商 rproc 没开
# CONFIG_AW_RPBUF_* is not set          ← 全系列没有
# CONFIG_AW_RPMSG_* is not set          ← 全系列没有
```

驱动实际绑定（`/sys/bus/platform/drivers/sunxi-msgbox/`）：
`3004000.msgbox`，中断 432/433 已注册，**计数 0/0**——初始化完成，一次中断都没收到过。

### 核查 6：固件

```
find /lib/firmware /boot -iname "*e902*" -o -iname "*riscv*" -o -iname "*e906*"
→ 空
```

**板上没有任何可加载的协处理器固件。**

### 对那份「Tina SDK 有完整支持」材料的判定

| 材料说法 | 实测 |
| --- | --- |
| 有 `sunxi_rproc_firmware.c` 等驱动源码 | ❌ 树里**没有任何 .c** |
| 标准 remoteproc 框架可用 | ❌ `CONFIG_REMOTEPROC` not set |
| `sunxi_rpmsg` 驱动 + `/dev/rpmsg_ctrlX` | ❌ 无 `/dev/rpmsg*`，无 `/sys/class/remoteproc` |
| 固件格式 `.fex` | ❌ **硬错**。`.fex` 是全志文本配置格式（`sys_config.fex` / `sys_partition.fex`），经 `fex2bin` 转 `script.bin` 给 bootloader 读，**不是可执行代码** |
| Melis RTOS 适配 | ❓ 无法验证（板上无任何固件） |

但那份材料也有**方向正确**的部分：`aw_rpmsg.h` 里的
`CONFIG_AW_RPMSG_NOTIFY` / `CONFIG_AW_RPROC_FAST_BOOT` 说明全志确实有
这套 AMP 基础设施，只是**用在别的芯片上**（sun55iw5 等），没给 sun60iw2。

**判定：该材料描述的软件栈在全志其他芯片上可能存在，但在 sun60iw2 上不存在板级支持。它的细节无法验证（.c 文件不存在），其中一处可判定为硬错误（.fex）。**

## 结果

### 结论

**路线 1（E902 协处理器）在 sun60iw2 上不可行。** 缺的不是驱动源码，是**硬件布局**：

| 缺什么 | 后果 |
| --- | --- |
| E902 的 SRAM 基地址 | 不知道固件往哪搬、virtio ring 放哪 |
| E902 的启动方式（复位释放序列） | 无法把核从 reset 里放出来 |
| 板级时钟/复位定义 | `sun60iw2` 头文件里连名字都没有 |
| 固件 + 工具链 | RISC-V 交叉工具链与 RTOS 移植 |
| 调试通道 | 没有 JTAG、没有串口，跑飞了看不见 |

**即使原厂驱动全开源也给不了前三项**——它们只存在于原厂文档/SDK 里。

### 一个遗留异象（记录但不影响结论）

`sun60iw2-r-ccu.h` 的 include guard 是 `_DT_BINDINGS_CLK_SUN60IW1_R_CCU_H_`，
文件头注释是 `Copyright (c) 2023 rengaomin@allwinnertech.com`——说明这颗芯片的
R-CCU 头文件是从 `sun60iw1` 复制来的。而 `sun65iw1-r-ccu.h` 里明确有 E902。

**推论（未经证实）**：硬件上 R-CCU 域可能确实存在 RISC-V 核（`CLK_RISCV*` 三件套
的命名与 `CLK_E902*` 三件套结构完全对应），但**这棵内核树没有为它写板级支持**。

这不改变结论：**没有板级定义 = 无法加载固件 = 无法使用。**

### 建议路径

**路线 2：A55 隔离 AMP。** 已核查的硬件事实支持这条路：

| 事实 | 值 |
| --- | --- |
| CPU | 8× Cortex-A55，**2 socket × 4 core** |
| 当前 cmdline | 无 `isolcpus` |
| cpu6 / cpu7 当前频率 | 1.404 GHz / 1.014 GHz（低于 cpu0 的 1.794 GHz） |
| `reserved-memory` | 只有 `bl31`，但可自行加 CMA/预留 |
| `CONFIG_MAILBOX=y` + `sunxi-msgbox` 已绑定 | 门铃现成可用 |

路线 2 不需要任何未公开知识：`isolcpus` + 一块预留内存 + 字符设备 mmap 导出即可验证骨架。
注意 **cpu6/cpu7 属第二个 socket 且降频**，选核时要实测延迟而非只看核号。

### 遗留边界

- 未拿到全志 Tina SDK，无法 100% 排除「原厂另有支持」。但**即使有，也需先拿到
  SRAM 布局文档**，成本远高于路线 2。
- 向 Radxa / 全志提问仍未做（成本最低的一步，建议做）。
- `docs/ideas/A733跨核rpmsg通信-可行性与路线.md` 里
  「`CONFIG_AW_MSGBOX` not set」的结论已过期（实测 `=y` 且驱动已绑定），需同步。
