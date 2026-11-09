# 阶段 2 overlay 演练手册（真正动手前的可执行清单）

> 日期：2026-11-08
> 分类：工作流外实施（演练前置准备，尚未执行）
> 结果：清单已完成，**演练本身未开始**——板子当前不在线

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

方案 §10 把 USB-UART 串口线称作「阶段 2 的变砖保险」。但这个保险**从未在真机上
验证过能连上**——线买了、在手里，但没插到板子上读过一次启动日志。

这是一处纸面保险。若跳过这步直接动 overlay，一旦板子起不来，而串口也没通，
就退回到「拔 eMMC/SD 卡挂到另一台机器上改」的境地，风险等级完全不同。

所以本手册把「串口线验证」列为 **Step 0**，它是整个阶段 2 的前置门槛，
不通过就不进入 Step 1。

### 关键取舍三：为什么不用 `rsetup` 交互界面

板端有 `/usr/bin/rsetup`，它是 Radxa 官方的 overlay 配置入口。但它是**交互式
TUI**，通过 SSH 驱动它需要终端多路复用，且其内部实际动作就是
`mv .disabled` + `u-boot-update`。

直接执行这两条命令：可脚本化、可审计、可精确回滚。TUI 只适合人手操作。

## 实现方式

### 当前板端事实（2026-11-07 实测回读）

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

物理接线（需人在板子旁操作）：

```
USB-UART 线的 TX  → 板子 40pin 排针的 UART RX
USB-UART 线的 RX  → 板子 40pin 排针的 UART TX
USB-UART 线的 GND → 板子 GND
USB-UART 线的 VCC → 不接（板子自己供电，不要双供电）
```

注意板上 console 是 `console=ttyAS0,115200n8`，需要确认 `ttyAS0` 对应哪组排针引脚。
**这一步若无法确定引脚定义，必须先查 Radxa Cubie A7A 官方 pinout 文档，
不得靠猜。接错 TX/RX 只是不通，接错 VCC/GND 可能损伤板子。**

在宿主机执行（端口名按实际调整，常见 `/dev/ttyUSB0`）：

```bash
# 确认线被识别
ls -l /dev/ttyUSB*

# 打开串口，波特率必须与 console= 参数一致
sudo picocom -b 115200 /dev/ttyUSB0
# 或： sudo minicom -D /dev/ttyUSB0 -b 115200
```

**判据**：串口终端里有输出。若板子正在运行，敲回车应能看到登录提示；
若无输出，可 `sudo reboot`（通过 SSH）观察是否有 U-Boot 启动日志刷出。

**通过条件**：能看到 U-Boot 或内核日志，且能敲入字符。

**未通过时**：停止。不要进入 Step 1。排查项：TX/RX 是否接反、波特率、
3.3V 电平（CH340 有的是 5V）、引脚定义是否正确。

### Step 1：基线快照（只读，可随时重做）

```bash
# 在宿主机上，把关键启动文件拉到本地留档
mkdir -p ~/board-baseline-$(date +%Y%m%d)
scp board:/boot/extlinux/extlinux.conf ~/board-baseline-$(date +%Y%m%d)/
ssh board 'cat /boot/dtbo/managed.list' > ~/board-baseline-$(date +%Y%m%d)/managed.list

# 记录当前设备树节点状态（用于对比 overlay 生效前后的差异）
ssh board 'ls /proc/device-tree/ | sort' > ~/board-baseline-$(date +%Y%m%d)/dt-nodes-before.txt
ssh board 'ls /dev/ttyS* /dev/ttyAS* 2>/dev/null' > ~/board-baseline-$(date +%Y%m%d)/tty-before.txt
```

**判据**：本地有 4 个文件，且 `extlinux.conf` 非空。

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

**通过条件（三条全部满足）**：
1. SSH 能连上，`uptime` 显示刚启动；
2. `/proc/device-tree/` 下出现 uart2 相关节点（用 Step 1 的 `dt-nodes-before.txt` 对比）；
3. `/dev/ttyS*` 或 `/dev/ttyAS*` 数目比 Step 1 多。

```bash
ssh board 'ls /proc/device-tree/ | sort' > /tmp/dt-after.txt
diff ~/board-baseline-*/dt-nodes-before.txt /tmp/dt-after.txt
```

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
2. uart2 节点消失（`diff` 结果回到基线）；
3. 系统其余一切正常。

**通过条件**：三条全部满足。至此 overlay 的「启用 → 生效 → 撤回 → 恢复」闭环
全部验证通过。

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
4. 若 U-Boot 菜单都进不去（黑屏/无输出），用 Step 1 的备份在另一台机器上
   修改 `extlinux.conf` 后写回启动介质。

**若串口也完全无输出**：说明 Step 0 的验证没做或没通过，此时只能拔卡。
**这正是 Step 0 设为门槛的原因。**

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

### 未做（受阻塞）

- **演练本身未开始**。板子当前不在线（`ssh board` 超时，隧道端口仍在监听，
  说明板端 sshd 未响应）。且 Step 0 需要人物理接线，无法远程完成。
- **串口线仍未验证**。这是当前最值得警惕的一处纸面保险。

### 诚实边界

1. 本文档里的板端事实全部来自 2026-11-07 的命令回读，**今天（2026-11-08）
   未能重新核对**，因为板子不在线。执行前应重跑一次 Step 1 的基线快照。
2. Step 0 的排针引脚定义我**没有核实**。文中明确写了「若无法确定必须先查官方
   pinout，不得靠猜」——这不是推卸，而是接线错误可能损伤硬件。
3. 「uart2 overlay 启用后只多一个串口节点」这一判断基于 dtbo 名称与实际
   影响面的推断，**未在真机验证**。这正是 Step 3 要做的事。

### 下一步

1. 板子恢复在线 → 重做 Step 1 基线快照（确认事实未漂移）
2. 人在板边 → 完成 Step 0 串口验证（这是硬门槛）
3. Step 2-4 → 验证 overlay 机制的启用与回滚闭环
4. Step 6 → 摄像头 overlay，之后即可接入 `V4l2FrameSource` 真机调通
