# 阶段 2 overlay 演练手册（真正动手前的可执行清单）

> 日期：2026-11-08（2026-11-09 更新：Step 0 实测通过、五处判据订正、引脚争用判断修正）
> 分类：工作流外实施（演练前置准备，尚未执行）
> 结果：**Step 0 串口门槛已通过**；Step 1 基线部分抓取（缺 uart 状态，已补采集命令但未重跑）；
> Step 2-6 未执行（板子已被用户关闭）。引脚争用**不再视为 Step 2 的前置阻塞**，
> 但仍是 Step 6 的安全前提（见「关于引脚争用的判断修正」）

## 构思

### 为什么需要这份文档

`驱动面能力引入与业务融合方案.md` §11.1 把「阶段 2 overlay 演练」排在「阶段 3
摄像头」之前，理由是 §11.2 风险表的第一条：

```
| overlay 启用导致启动失败 | 板子离线 | 串口线 + rescue 标签；一次只改一个 overlay |
```

摄像头 overlay **本身就是那个有变砖风险的动作**。如果直接上摄像头 overlay 而板子
起不来，就无法区分是「overlay 机制不可用」还是「摄像头那份 dtbo 有问题」——
两个不同量级的故障共用一个症状，现场无从下手。

所以正确顺序是：先拿一份**失败也不影响启动**的 overlay 验证整条机制
（`mv .disabled` → `u-boot-update` → 重启 → 节点出现 → 回滚），确认无误后再动
摄像头那份。

### 为什么不做进 OpenSpec change

用户 2026-11-06 明确要求「以后只要我不让你新开 change，你就默认不用新开」
（memory: `no-new-openspec-change-by-default`）。本任务是演练准备，
按工作流外实施推进，**不具备 OpenSpec 验收效力**。

### 关键取舍一：首测选哪一份 overlay

| 候选 | 启用后影响 | 判定 |
| --- | --- | --- |
| `sun60iw2p1-uart2.dtbo` | 多一个串口设备节点 | ✅ **选它** |
| `cubie-a7a-enable-sunxi-ac101-sound-card.dtbo` | 多一块声卡 | 次选。但音频子系统初始化路径更长，失败面更大 |
| `sun60iw2p1-i2s4-2ch.dtbo` | I2S 通路 | ❌ 与音频耦合，牵连面不明 |
| `cubie-a7a-radxa-camera-8m-219.dtbo` | CSI/ISP/VIN 全链路 | ❌ **这就是要验证的目标，不能当首测** |
| `cubie-a7a-radxa-display-*.dtbo` | DSI 显示 | ❌ 会改 tcon/de 时序，风险高于收益 |

**选 `uart2` 的理由**：它只增加一个串口控制器节点，不触碰存储、显示、网络、
电源管理。即使 dtbo 本身有错，最坏结果是「多了个不工作的串口」，而不是
「起不来」。而且它的正确性判据极其明确——`/dev/ttyS*` 数目变化。

### 关键取舍二：串口线必须先单独验证

方案 §10 把 USB-UART 串口线称作「阶段 2 的变砖保险」。但这个保险在写下这些字的时候
**从未在真机上验证过**——线买了、在手里，但没插到板子上读过一次启动日志。

这是一处纸面保险。若跳过这步直接动 overlay，一旦板子起不来，而串口也没通，
风险等级完全不同（本板存储是 UFS 焊死，连「拔卡改配置」的退路都没有）。

所以本手册把「串口线验证」列为 **Step 0**，它是整个阶段 2 的前置门槛，
不通过就不进入 Step 1。

**2026-11-09 更新：这一步已实测通过**，纸面保险变成了已验证的保险。
证据见 Step 0「实测记录」。

### 关键取舍三：为什么不用 `rsetup` 交互界面

板端有 `/usr/bin/rsetup`，它是 Radxa 官方的 overlay 配置入口。但它是**交互式
TUI**，通过 SSH 驱动它需要终端多路复用，且其内部实际动作就是
`mv .disabled` + `u-boot-update`。

直接执行这两条命令：可脚本化、可审计、可精确回滚。TUI 只适合人手操作。

## 实现方式

### 当前板端事实（2026-11-07 实测回读；关键项 2026-11-09 复核）

```
/boot/dtbo/ 下 20 份 dtbo，全部 .disabled 后缀
/boot/dtbo/managed.list 含 20 行，与 dtbo 一一对应
/boot/uEnv.txt 已退役，内容为：
    # uEnv.txt has been retired.
    # Please use:
    #   `rsetup` command to confiure device overlays;
    #   `sudo nano /etc/kernel/cmdline && sudo u-boot-update` to update kernel arguments.
/boot/extlinux/extlinux.conf 由 u-boot-update 生成，
    prompt 1 / timeout 10，含 l0 / l0r(rescue) / l1 / l1r(rescue)
    当前内核 5.15.147-99btf1-a733，l1 指向 5.15.147-21-a733
/dev/video* /dev/media* : 不存在（摄像头未启用）
card0-HDMI-A-1 : disconnected
```

`l0r` 是 rescue 标签（`append ... single`），这是启动失败时的第一道退路。

### Step 0：验证串口线能连（**前置门槛，不通过则停止**）

> ✅ **2026-11-09 已通过**。证据见下方「实测记录」。以下保留原始步骤供复盘。

**关键订正**：console 走的是 **uart0（`2500000.uart` → `/dev/ttyAS0`）**，
不是 40pin 上的 uart2。二者是**不同的硬件口**：

| | uart0 | uart2 |
| --- | --- | --- |
| 地址 | `2500000` | `2501000` |
| 设备节点 | `/dev/ttyAS0` | 启用 overlay 后才有 |
| 用途 | **console**（U-Boot + 内核日志 + 登录） | 普通串口外设 |
| 引脚 | 板载调试口 | PB0/PB1 = 40pin 的 pin 7 / pin 11 |
| 当前状态 | `okay` | `disabled` |

**救砖必须接 uart0**，因为只有它会输出 U-Boot 阶段的信息、并能选择
启动菜单里的 rescue 标签。接 uart2 只能验证 overlay 生效，救不了砖。

物理接线（需人在板子旁操作）——接板载调试排针，**不要接 40pin**：

```
USB-UART 线的 TX  →  板子调试口的 RX
USB-UART 线的 RX  →  板子调试口的 TX
USB-UART 线的 GND →  板子调试口的 GND
USB-UART 线的 VCC →  不接（板子自己供电，不要双供电）
```

TX/RX 必须交叉。接错 TX/RX 只是不通，**接错 VCC/GND 可能损伤板子**。

在**你自己的电脑**上（不是 Cloud 宿主）打开串口，波特率必须与 `console=` 一致：

```
115200 8N1，无流控
```

Windows 用 PuTTY（Connection type 选 **Serial**，不是 SSH）。
Linux/Mac 用 `picocom -b 115200 /dev/ttyUSB0`。

**通过条件（三条全部满足）**：
1. 能看到 **U-Boot 启动信息**（不只是内核日志）；
2. 能看到 **U-Boot 启动菜单**（含 rescue 标签）；
3. 能登录系统（说明双向通信正常）。

**未通过时**：停止。不要进入 Step 1。排查项：TX/RX 是否接反、波特率、
3.3V 电平（CH340 有的是 5V）、是否接错口（uart2 而非 uart0）。

#### 实测记录（2026-11-09）

用户接板载调试口，Windows + PuTTY 115200，`sudo reboot` 后捕获到完整链路：

```
U-Boot 2018.07-17-boot-aw2501-g4c7078cfdf7-dirty (Apr 29 2026 - 10:03:59 +0000)
[01.501]Model: Radxa Cubie A7A
[01.568]DRAM:  8 GiB
...
U-Boot menu
1:      Debian GNU/Linux 11 (bullseye) 5.15.147-99btf1-a733
2:      Debian GNU/Linux 11 (bullseye) 5.15.147-99btf1-a733 (rescue target)
3:      Debian GNU/Linux 11 (bullseye) 5.15.147-21-a733
4:      Debian GNU/Linux 11 (bullseye) 5.15.147-21-a733 (rescue target)
Enter choice: 1:        Debian GNU/Linux 11 (bullseye) 5.15.147-99btf1-a733
...
[10.846]Starting kernel ...
```

**四条判据全部满足**：U-Boot 输出可见 ✅、启动菜单可见 ✅、
`rescue target` 标签可选中 ✅、登录成功 ✅。

板端核对：`who` 显示 `radxa ttyAS0 2026-11-09 04:35`，即当前登录会话走的是
`/dev/ttyAS0`。结合内核参数 `console=ttyAS0,115200n8` +
`earlyprintk=sunxi-uart,0x2500000`，可以确认用户接的是 console 口。

> 待补：`readlink /sys/class/tty/ttyAS0/device` 应指向 `2500000.uart`，
> 这条命令本轮未执行（`ssh` 当时已不通），执行 Step 2 前顺手确认一次。

**结论：变砖保险从「纸面」变为「已验证」，可以进入 Step 1。**

### Step 1：基线快照（只读，可随时重做）

```bash
# 在宿主机上，把关键启动文件拉到本地留档
mkdir -p ~/board-baseline-$(date +%Y%m%d)
scp board:/boot/extlinux/extlinux.conf ~/board-baseline-$(date +%Y%m%d)/
ssh board 'cat /boot/dtbo/managed.list' > ~/board-baseline-$(date +%Y%m%d)/managed.list

# 记录当前设备树节点状态（用于对比 overlay 生效前后的差异）
ssh board 'ls /proc/device-tree/ | sort' > ~/board-baseline-$(date +%Y%m%d)/dt-nodes-before.txt
ssh board 'ls /dev/ttyS* /dev/ttyAS* 2>/dev/null' > ~/board-baseline-$(date +%Y%m%d)/tty-before.txt

# 【必须补】uart2 的 status 快照——它才是 Step 3 判据 2 的比对对象。
# 为什么单独一条：上面那个 ls 只列**顶层**节点，而 uart@2501000 埋在
# soc@3000000 下面，顶层清单里根本看不到它（2026-11-09 实测确认：
# 51 个顶层节点里与 uart 相关的只有 soc@3000000 这一行）。
# 漏了这一条，Step 3 的判据 2 就没有基线可比，等于白做。
ssh board 'for n in $(find /proc/device-tree -name "uart@2501*" 2>/dev/null); do
  echo "$n = $(tr -d "\0" < "$n/status")"
done' > ~/board-baseline-$(date +%Y%m%d)/uart2-status-before.txt

# 顺手留一份 uart0（console）的 pinctrl 指纹，供日后查引脚争用时比对
ssh board 'for n in $(find /proc/device-tree -name "uart@2500*" 2>/dev/null); do
  echo "$n = $(tr -d "\0" < "$n/status")"
done' >> ~/board-baseline-$(date +%Y%m%d)/uart2-status-before.txt
```

**判据**：本地有 5 个文件，`extlinux.conf` 非空，且
`uart2-status-before.txt` **非空并含 `uart@2501`**（若为空，说明
`find` 没定位到节点，必须先查清节点真实路径再往下走）。

> **这一条是 2026-11-09 补的**。原 Step 1 只抓了顶层节点清单，
> 而 overlay 改的是 `soc@3000000/uart@2501000` 的 `status`，顶层清单
> 看不到它。若不补这条基线，Step 3 的判据 2 无基线可比。

### Step 2：启用 uart2 overlay

```bash
ssh board 'sudo mv /boot/dtbo/sun60iw2p1-uart2.dtbo.disabled /boot/dtbo/sun60iw2p1-uart2.dtbo'
ssh board 'sudo u-boot-update'
```

`u-boot-update` 会重新生成 `/boot/extlinux/extlinux.conf`，在 `l0` 段加入
`fdtoverlays` 行。

**判据（重启前）**：
```bash
ssh board 'grep -A2 fdtoverlays /boot/extlinux/extlinux.conf'
```
应能看到 `fdtoverlays` 指向 uart2。

### Step 3：重启并验证

**重启前确认串口终端已打开并正在录制输出**——这是能救砖的前提。

```bash
ssh board 'sudo reboot'   # 连接会断开，属正常
```

等待约 60 秒后尝试重连：

```bash
for i in $(seq 1 6); do
  echo "--- try $i ---"
  timeout 20 ssh -o ConnectTimeout=10 -o BatchMode=yes board 'uptime' && break
  sleep 10
done
```

**通过条件（四条全部满足）**：
1. SSH 能连上，`uptime` 显示刚启动；
2. `uart@2501000` 的 `status` 由 `disabled` 变为 `okay`；
3. `/dev/ttyAS*` 数目比 Step 1 多（预期新增 `ttyAS2` 之类）。
4. **console 仍然可用**（判据 4，见下方「关于判据 4」）。

**关于判据 4**：这条是 2026-11-09 补的，专门用来回答「uart2 是否抢走了
console 的 PB0/PB1」。**它不阻塞 Step 2，但它决定 Step 6 能否安全执行。**

判定方式（无需额外硬件，就在串口终端里看）：

- 启动过程中串口终端**是否仍有 U-Boot 菜单 + 内核日志**刷出？
- 启动完成后在串口终端敲回车，**是否还能拿到 `login:` 提示 / shell 提示符**？

三种结果对应的处置：

| 判据 4 结果 | 含义 | 处置 |
| --- | --- | --- |
| 串口全程正常 | uart2 未争用引脚（或争用但未生效） | ✅ 直接进 Step 4；救砖通道完好，Step 6 可安全做 |
| 串口完全无输出，但 SSH 通 | **uart2 抢走了 console 引脚** | ⚠️ 记录该事实。**Step 4 回滚后必须再确认串口恢复**；在确认恢复前**不得执行 Step 6** |
| 串口有输出但登录不进去 | 串口设备在，但 getty 起在别处 | 用 SSH 查 `systemctl status serial-getty@ttyAS0`，按实际情况记录 |

**判据 4 失败不等于演练失败**——它恰恰是这个演练最有价值的产出：用一次
低风险操作，把「uart2 与 console 的引脚关系」这个此前只能靠反编译推测的问题，
变成了实测结论。

**订正（2026-11-09 复盘）**：原先写的判据是「`/proc/device-tree/` 下出现
uart2 相关节点」，这是**错的**。实测 `dtc` 反编译 dtbo 可见它并不创建新节点，
而是对已存在的 `uart@2501000` 做 `status = "okay"` 覆盖——节点一直在，
只是被禁用。所以拓扑 diff 会显示「无差异」而实际已生效，按原判据会误判失败。

```bash
# 判据 2：status 翻转——直接与 Step 1 新增的基线文件比对
ssh board 'for n in $(find /proc/device-tree -name "uart@2501*" 2>/dev/null); do
  echo "$n = $(tr -d "\0" < "$n/status")"
done' > /tmp/uart2-status-after.txt

diff ~/board-baseline-*/uart2-status-before.txt /tmp/uart2-status-after.txt
# 期望只有一处差异：disabled → okay

# 判据 3：串口设备节点
ssh board 'ls /dev/ttyAS*'
```

> 为什么不写死 `soc@3000000`：不同内核版本下 `soc` 节点名可能带不同 unit-address，
> 写死会让执行者拿到 `No such file` 并误以为 overlay 没生效。`find -name 'uart@2501*'`
> 对节点名变化免疫。

### Step 4：回滚演练（**必须做，不能跳过**）

这是整个阶段 2 最有价值的一步：**在一切正常时，验证回滚确实可用**。
如果只验证「启用成功」而不验证「能否撤回」，那么这份保险从未被检验过。

```bash
ssh board 'sudo mv /boot/dtbo/sun60iw2p1-uart2.dtbo /boot/dtbo/sun60iw2p1-uart2.dtbo.disabled'
ssh board 'sudo u-boot-update'
ssh board 'sudo reboot'
```

重启后确认：
1. SSH 能连上；
2. `uart@2501*` 的 `status` **回到 `disabled`**（与 Step 1 基线一致）——
   注意**不是「节点消失」**，节点一直存在（理由同 Step 3 的订正）；
3. 系统其余一切正常，**且串口 console 仍然可用**（若 Step 3 判据 4 曾失败，
   这一步必须看到串口恢复才算回滚成功）。

**通过条件**：三条全部满足。至此 overlay 的「启用 → 生效 → 撤回 → 恢复」闭环
全部验证通过。

> **回滚后必做**：在串口终端里敲一次回车，确认拿到 `login:` / shell 提示符。
> 如果 Step 3 期间 console 曾失效、而回滚后**仍未恢复**，说明引脚已被 uart0
> 之外的某个状态占住——此时不要重启，用 SSH 检查并把现场结果带回来分析。
> 这是本演练唯一可能需要停下来问人的情形。

### Step 5：启动失败时的救援流程（希望用不到，但必须写下来）

若 Step 3 之后 SSH 连不上：

1. **串口终端里看卡在哪**。`prompt 1` + `timeout 10` 意味着 U-Boot 菜单会停 10 秒，
   此时按任意键可停在菜单。
2. **按方向键选 `l0r`**（rescue target，带 `single` 参数），回车启动。
3. rescue 模式下 root 免密登录，然后：
   ```bash
   mv /boot/dtbo/sun60iw2p1-uart2.dtbo /boot/dtbo/sun60iw2p1-uart2.dtbo.disabled
   u-boot-update
   reboot
   ```
4. 若 U-Boot 菜单都进不去（黑屏/无输出），这就是最坏情况。**注意：本板存储是
   UFS（焊死），没有 eMMC/SD 卡可拔**——串口日志里 `ufs has init` +
   `No MMC device available` 已证实这一点。所以「拔卡挂到另一台机器改」这条
   经典退路**在这里不存在**。

   真正的退路只有两条：
   - U-Boot 命令行（串口停在菜单后按 `c` 进 console），用 `fatls mmc 0` /
     `ext4ls` 之类命令定位并 `fatwrite`/`ext4write` 改回 `extlinux.conf`；
   - 若 U-Boot 本身都挂了，只能走 USB 烧录（Allwinner FEL 模式）重刷整机镜像，
     代价是丢系统盘数据。**这就是为什么一次只改一个 overlay、且必须先做
     Step 4 回滚演练。**

**若串口也完全无输出**：此时既无串口日志、也无 SD 卡可拔，唯一的物理入口只剩
FEL/USB 烧录。**这正是 Step 0 设为硬门槛的原因——它没有备份方案。**

### Step 6：摄像头 overlay（Step 1-4 全部通过后才做）

```bash
ssh board 'sudo mv /boot/dtbo/cubie-a7a-radxa-camera-8m-219.dtbo.disabled /boot/dtbo/cubie-a7a-radxa-camera-8m-219.dtbo'
ssh board 'sudo u-boot-update'
ssh board 'sudo reboot'
```

重启后：

```bash
# 1. 设备树节点
ssh board 'ls /proc/device-tree/ | grep -i "vind\|csi\|isp\|mipi"'

# 2. 驱动模块
ssh board 'lsmod | grep -i "vin\|imx219"'
ssh board 'sudo modprobe vin_v4l2; sudo modprobe imx219; lsmod | grep -i "vin\|imx219"'

# 3. video 节点
ssh board 'ls -l /dev/video* /dev/media*'

# 4. 格式枚举（需先装 v4l-utils）
ssh board 'sudo apt install -y v4l-utils && v4l2-ctl --list-devices'
ssh board 'v4l2-ctl -d /dev/video0 --list-formats-ext'
```

**通过条件**：`/dev/video*` 出现，且 `v4l2-ctl --list-formats-ext` 能列出
至少一种格式（NV12 或 YUYV）。

**未通过时**：不要现场硬调。按 Step 4 方式回滚，把 `dmesg | grep -i "imx219\|vin"`
的输出带回分析。

## 结果

### 已完成

- 首测 overlay 选定为 `sun60iw2p1-uart2`，理由与备选对比已写入本文档
- 六步执行清单（含 Step 0 串口门槛）已写成可直接照抄的命令
- 回滚路径与救援流程已写明，且回滚演练被列为必做项
- 同步完成 `V4l2FrameSource` 真机实现（10 项单测全绿，全量 ctest 60/60）
- **Step 0 串口门槛已通过（2026-11-09）**，证据见 Step 0「实测记录」：
  U-Boot 输出 / 启动菜单 / rescue 标签 / 登录，四项判据全部实测满足
- Step 1 基线快照**部分**抓取（`~/board-baseline-20261109-1213/`）：
  `extlinux.conf`、`managed.list`、`dt-nodes-before.txt`（51 个顶层节点）、
  `tty-before.txt`（仅 `/dev/ttyAS0`）。
  **缺 `uart2-status-before.txt`**——这是 2026-11-09 复查时才发现的设计缺陷：
  顶层节点清单看不到埋在 `soc@3000000` 下的 `uart@2501000`，Step 1 已补上
  这条采集，但**尚未在板上重跑**（板子已被用户关闭）。
- 手册四处已知错误已订正（见下方「本轮订正」）

### 本轮订正（2026-11-09）

| 位置 | 原文（错） | 改为（实测依据） |
| --- | --- | --- |
| Step 0 接线 | 接 40pin 排针的 UART RX/TX | 接**板载调试排针** → uart0 → `/dev/ttyAS0`。40pin 上那组是 uart2（pin 7/11），**不是 console，救不了砖** |
| Step 0 执行位置 | 宿主机 `ls /dev/ttyUSB*` + `picocom` | 在**用户自己的 Windows 电脑**上用 PuTTY（Serial / COMx / 115200 8N1）。Cloud 宿主看不到串口设备 |
| Step 1 基线 | 只抓 51 个**顶层** dt 节点 | 必须**额外**抓 `uart2-status-before.txt`。`uart@2501000` 埋在 `soc@3000000` 下，顶层清单看不到它，原基线无 uart 状态 |
| Step 3 判据 2 | `/proc/device-tree/` 下「出现」uart2 节点 | `uart@2501*` 的 `status` 由 `disabled` 翻为 `okay`。**overlay 不创建新节点**，只改已有节点的 status，按原判据会误判失败 |
| Step 3 通过条件 | 三条（无 console 检查） | **加到四条**，判据 4 显式检查串口 console 是否仍可用——这是回答引脚争用的实测手段 |
| Step 4 判据 2 | 「uart2 节点消失」 | `status` **回到 `disabled`**。节点不会消失，同 Step 3 的道理 |
| Step 5 救援 | 「拔 eMMC/SD 卡挂到另一台机器改」 | **本板是 UFS（焊死），没有卡可拔**。退路只剩 U-Boot 命令行改 `extlinux.conf`，或 FEL/USB 重刷整机镜像 |
| 诚实边界 1 | 「Step 2 之前必须先解决引脚争用」 | **降级为不阻塞 Step 2**，见下方「关于引脚争用的判断修正」 |

### 关于引脚争用的判断修正（2026-11-09）

**原判断**：uart2 可能与 console 争用 PB0/PB1，若争用则 console 失效、救砖通道被毁，
所以必须先反编译 dtbo 查明，才能执行 Step 2。我据此把 Step 2 挂起了。

**用户判断**：「感觉好像没有必要解决这个问题」。**我复核后同意，原判断是过度保守。**

错在哪里——我把两件事混成了一件：

| 事件 | 会不会发生 |
| --- | --- |
| uart2 抢走 console 引脚（若有争用） | 可能 |
| **板子起不来** | **不会**。一个 UART 节点的 `status` 翻转不构成启动失败的成因 |

而**救砖通道（串口 + rescue 标签）只在「板子起不来」时才需要**。
若只是 console 失效而板子正常启动，SSH 照样能连，用 SSH 回滚即可，几十秒的事。

所以这个疑问不值得用 `dtc` 反编译去**提前取证**——Step 2 本身就是成本更低的
实测手段：跑一次，看串口还活着没有，答案立刻就有了。这正是演练的价值：
用一次低风险操作替代一轮纸上推演。

**但原判断里有一条是对的，不能一并丢掉**：

> 若 Step 2 真的让 console 失效，那么在 Step 6（摄像头 overlay，
> 那个真正有变砖风险的动作）之前**必须先把救砖通道修回来**。

风险本身没有消失，只是从 Step 2 转移到了 Step 6。因此 Step 3 增加了判据 4
（显式检查 console 是否仍可用），Step 4 增加了「回滚后必须确认串口恢复」。

**一句话总结**：争用问题不阻塞 Step 2，但它决定 Step 6 的安全前提。
所以不提前取证，但必须在 Step 3/4 里实测并记录。

- **Step 2-6 尚未执行**。用户在 2026-11-09 关闭了板子，本轮无法执行。
  执行 Step 2 需要两个条件同时具备：板子开机、人在板边且串口终端开着并录制。

### 诚实边界

1. **uart2 与 uart0 可能争用同一组引脚，此问题仍未查清——但它不阻塞 Step 2**。
   板端 dmesg 有：
   ```
   uart-ng 2500000.uart: there is not valid maps for state default/sleep
   sunxi:pin-2000000.pinctrl:[ERR]: unknown pin   (×4)
   ```
   而 `sun60iw2p1-uart2.dtbo` 里写着 `exclusive = "PB0\0PB1\0uart2"`、
   `pins = "PB0\0PB1"`，同时 `uart0_pins@0` 也带 `pins = [00 00]`
   （sunxi 二进制编码，大致对应 PB0/PB1）。

   我**无法从现有证据判断** uart2 overlay 生效后是否会夺走 uart0 的引脚。

   **但这个问题不阻塞 Step 2**（2026-11-09 用户判断，我复核后同意）。理由是
   区分「最坏结果」与「启动失败」两件事：

   | 情形 | 板子启动 | SSH | console | 处理 |
   | --- | --- | --- | --- | --- |
   | uart2 拿不到引脚，probe 失败 | ✅ 正常 | ✅ | ✅ 正常 | 记日志，Step 3 判据 2 照看 status |
   | uart2 抢走 PB0/PB1，console 死 | ✅ 正常 | ✅ | ❌ 失效 | **SSH 回滚**（Step 4），30 秒 |

   两种情形板子都照常启动。而**救砖通道只在「板子不启动」时才需要**——
   一个 UART 节点的 `status` 翻转不构成启动失败的成因。所以这个疑问用
   Step 2 本身花几十秒就能实测回答，不必先做 `dtc` 反编译取证。

   **真正的影响在 Step 6，不在 Step 2**：若 Step 2 之后 console 确实失效，
   那么在 Step 6（摄像头 overlay，那个真正有变砖风险的动作）之前**必须先
   把 console 修回来**，否则等于在没有救砖通道的情况下做高风险动作。
   届时两条路：① 保持 uart2 禁用状态（回滚 Step 2）再动摄像头；
   ② 换首测候选（见「关键取舍一」，次选 `sunxi-ac101-sound-card`）验证机制。

   > 若日后仍想彻底查清争用，`dtc -I dtb -O dts` 反编译 dtbo 对比 `pinctrl-0`
   > 的 phandle 即可判定。这属于取证，不属于执行前置条件。

2. **`status` 的读取路径未在板端核实**。手册里用的是
   `find /proc/device-tree -name 'uart@2501*'`，避免写死 `soc@3000000`；
   但本轮 `ssh` 不通，我没能跑这条命令确认它真能定位到节点。
   执行 Step 1 时应顺手跑一次确认。

3. **「uart2 overlay 只多一个串口节点」仍是推断**。这正是 Step 3 要验证的事。

4. **Step 4 回滚演练的价值现在更高了**：因为本板没有可拔的存储卡，
   回滚路径一旦不可用，代价是重刷整机。所以 Step 4 不能因为「一切正常」而跳过。

### 下一步（按依赖顺序，不可跳步）

1. **板子开机后复核 Step 1 基线**（`~/board-baseline-20261109-1213/` 已存，
   重跑一遍确认未漂移），顺手跑一次
   `find /proc/device-tree -name 'uart@2501*'` 确认判据 2 的命令真能定位节点。
2. **人在板边 + 串口终端开着并录制** → 执行 Step 2（启用 uart2 overlay + `u-boot-update`）。
3. **Step 3** → 重启验证 `status` 翻为 `okay`、`/dev/ttyAS*` 增多，
   并**顺手确认 console 是否还活着**（判据 4，见 Step 3）。
4. **Step 4** → 主动回滚演练，验证可逆。**不可跳过。**
5. **Step 6** → 摄像头 overlay，之后 `V4l2FrameSource` 才能真机调通。
   **但先看 Step 3 的判据 4**：若 console 已失效，必须先恢复救砖通道再动摄像头。

> uart2 与 console 的引脚争用**不需要提前解决**——Step 2 本身就是它的实测手段，
> 最坏结果是 console 失效而板子仍正常启动，用 SSH 回滚即可（详见诚实边界 1）。
> 它真正约束的是 Step 6：高风险动作之前必须确认救砖通道还在。
