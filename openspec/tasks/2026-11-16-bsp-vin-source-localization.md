# IMX219 零帧：拿官方 BSP 源码做一次完整定位（含一次自我推翻）

> 日期：2026-11-16
> 分类：探索 / 排查（**未解决，根因仍未定位**）
> 结果：**部分完成** —— 推翻了本次自己提出的一条根因假设，确认了一处
> 真实存在的潜在缺陷，另外发现两处 BSP 新 bug；零帧根因**仍然开放**。
> 上板验证材料已备好，**等开发板在线后一次性执行**。

## 构思

### 起点

承接 [`2026-11-15-camera-zero-frame-imx219.md`](2026-11-15-camera-zero-frame-imx219.md)
与 [`2026-11-16-imx219-mclk-hypothesis-refuted.md`](2026-11-16-imx219-mclk-hypothesis-refuted.md)。
前两份记录之后，状态是：

- 软件配置层（模式表、寄存器、时钟、拓扑、格式协商）逐项排除；
- MCLK 假设被只读内核探针推翻（实测 24 MHz，与驱动宏一致）；
- 剩下的分界点是「sensor 没发」vs「SoC 的 PHY 没锁上」，两者都需要寄存器级证据；
- 但 `/sys/kernel/debug/mpp/mipi` 一读即 panic，`CONFIG_VIN_LOG` 又被编译掉，
  观测通道全断。

### 本次的目标

用户给出的约束是：**开发板当前不在线，先用官方 BSP 源码把问题完整定位，
等板子回来再一次性上板验证。**

于是目标不是继续猜，而是换一种取证方式：**不再依赖任何运行时日志，
而是从源码穷举「STREAMON 之后每一个阶段要执行，必须满足哪些条件」**，
把候选失败点收窄成一张可以用 kprobe 计数一次性判定的清单。

### 为什么不做进 OpenSpec change

纯排查，不产生生产代码变更；按 `openspec/tasks/README.md` 走本目录。

### 备选方案与取舍

| 方案 | 取舍 |
| --- | --- |
| 继续在板端试参数（改 DT、改 ioctl、换模式） | 否决。板子不在线；且没有判据，改了也不知道是哪一步生效 |
| 自写内核探针读 PHY 寄存器（上一轮遗留方案） | 保留但降级。能读到值，但**不知道读到的值该是什么**，仍缺语义 |
| **穷举源码条件 + kprobe 阶段计数** | **选中**。只读、不改状态、能一次性回答「哪一步没跑到」 |

关键判断：**「函数跑没跑过」比「寄存器值是多少」更接近根因**。
零帧的直接原因是「某一步没执行」，而不是「某个值不对」。

## 实现方式

### 一、把 s_stream 的整条链读通

逐段读了这些文件（BSP 副本在 `docs/hardware/radxa-cubie-a7a/bsp-vin/`）：

| 文件 | 读了什么 |
| --- | --- |
| `vin-tdm/vin_tdm.c` | `sunxi_tdm_subdev_s_stream` :897-1125、`sunxi_tdm_top_s_stream` :629-726、`tdm_set_rx_cfg` :601-627、`tdm_probe` :2473-2575、`tdm_isr` :2118-2148、四个 reset 函数 |
| `vin-csi/sunxi_csi.c` | `sunxi_csi_subdev_s_stream` :505-589、`sunxi_csi_s_mbus_config` :831-941、`__csi_init_subdev` :1023 |
| `vin-mipi/sunxi_mipi.c` | `combo_csi_mipi_init` :619-676、`combo_csi_init` :676-702、`sunxi_mipi_subdev_s_stream` |
| `vin-video/vin_video.c` | `vin_pipeline_set_mbus_config` :955-1030、`vidioc_set_tdm_speeddn_cfg` :3040-3075、`vin_set_tdm_speeddn_cfg_special` :5097-5136 |
| `vin_test/mplane_image/csi_test_mplane.c` | 厂商参考程序，确认它**没有**调用 `VIDIOC_SET_TDM_SPEEDDN_CFG` |

### 二、提出假设：`:1095` 的静默 `return 0`

`vin_tdm.c:1042-1097` 的 TDM_V200 线性模式下：

```c
if (!tdm->ws.speed_dn_en)
    return 0;                      /* ★ :1095-1096 */
```

返回 `0` 而非 `-EINVAL`，于是 `s_stream` → `vidioc_streamon` 全线成功，
但下面四行（`sunxi_tdm_top_s_stream` / `tdm_set_rx_cfg` /
`csic_tdm_rx_enable` / `csic_tdm_rx_cap_enable`）**一行都没执行**。

这能完美解释「零帧 + 零错误 + 零中断」，而且 `speed_dn_en` 确实默认为 0
（`kzalloc`，DT 不解析该键，驱动里自动使能的那段被注释掉在 :653-655），
唯一写入口是私有 ioctl。

### 三、自我推翻：判据来自错误的函数作用域

我用「板端日志里有 `__sunxi_tdm_reset_v2_handle:workqueue tdm_rx0 reset!!!`
这行打印」反推 `work_mode == TDM_ONLINE`（因为该函数里似乎有
`if (work_mode == TDM_OFFLINE) return;`），然后 ONLINE + `speed_dn_en==0`
⇒ 命中 `:1095`。

**这个推理的第 2 步是错的。** `:1479` 那句属于
`__sunxi_tdm_wdr_reset()`（:1465），不属于 `:1814` 打印所在的
`__sunxi_tdm_reset_v2_handle()`（:1746）。

后者的门是**间接**的：

```c
/* :1767 / :1850 */
if (!tdm->tdm_rx_reset[vind->vinc[i]->tdm_rx_sel])
    continue;
```

而 `tdm_rx_reset[] = 1` 的全部写点都在 `tdm_rx_set_reset()`（:2091-2114），
该函数第一句就是：

```c
#ifdef TDM_V200
    if (tdm->work_mode == TDM_ONLINE)
        return;          /* ONLINE 时永不置位 */
```

**所以那条打印只可能出现在 `TDM_OFFLINE`。板端有它 ⇒ 本板是 OFFLINE
⇒ `:1095` 不会执行 ⇒ 我的假设对本板不成立。**

这一课单独成文：
[`../pitfalls/two-similarly-named-reset-functions.md`](../pitfalls/two-similarly-named-reset-functions.md)。

### 四、顺带发现的两处 BSP 新 bug

在逐行读 `__sunxi_tdm_reset_v2_handle()`（:1746-1913）时撞到：

**1. 无配对的 `mutex_unlock`**

```
$ grep -n "graph_mutex" vin-tdm/vin_tdm.c
1892:	mutex_unlock(&vind->media_dev.graph_mutex);
```

全文件**只有这一处**。`__sunxi_tdm_reset_v2`（:1607-1745）里也没有对应的
`mutex_lock`。也就是说这个 work 在**解锁一把自己从未持有的互斥锁**。

后果取决于 `CONFIG_DEBUG_LOCK_ALLOC` / `CONFIG_DEBUG_MUTEXES`：开了会打
`DEBUG_LOCKS_WARN_ON`，没开则可能静默破坏 owner 字段。与本板零帧的
因果关系**未知**，但它是真实缺陷。

**2. 内层循环覆盖外层循环变量**

```
$ awk 'NR>=1746 && NR<=1913 /for *\(|mutex_/' vin-tdm/vin_tdm.c
1762: 	for (i = 0; i < VIN_MAX_DEV; i++) {
1789: 					for (i = 0; i < 200; i++) {
1798: 					for (i = 0; i < 200; i++) {
1845: 	for (i = 0; i < VIN_MAX_DEV; i++) {
1892: 	mutex_unlock(&vind->media_dev.graph_mutex);
```

同一个 `i` 被内外两层共用。内层跑完 `i == 200`，外层 `i++` 后直接跳出，
于是 **`VIN_MAX_DEV` 的遍历被截断成「只处理第一个 vinc」**。
这是「第二个及以后的 TDM 通道永远得不到复位」的典型来源。
本产品只用 channel 0，所以大概率不致命 —— 但这是运气，不是设计。

### 五、上板材料：`tools/board_vin_stage_probe.sh`

宿主机侧一次性探针，277 行，已 `chmod +x` 且 `bash -n` 通过。
（文件头已写明三条硬安全规则，此处不复述。）

做的事：

1. 预检 ssh / tracefs / `kprobe_events` 可写；
2. **只读**读 DT 的 `work_mode` 与 `device_id`
   （这是判据 C，把 ONLINE/OFFLINE 从「日志反推」升级为「直接读」）；
3. 装 12 个 kprobe，覆盖每个阶段入口；
4. 重启一次 `weaknet-server` 触发 open，观察 40s，收计数；**做两轮**
   （第二轮是为了顺带检验「重开一次就好」这个悬而未决的说法）；
5. 打差异表，并对 `overrun` 显式报警（计数是下界而非精确值）；
6. 给出固定判读顺序（先 `vin_tdm_rxen`，再 `vin_tdm_isr`，再 MIPI 两跳）。

三条设计约束值得单说：

- **探针装不上 ≠ 函数没被调用**。`static` 函数可能被内联，kprobe 注册会失败。
  脚本把失败项记为 `N/A (探针未安装)` 并单独列出，绝不并入零计数。
- **`/sys/kernel/debug/mpp/vi` 有条件地读**：只在服务刚 open 过时读一次，
  失败即放弃、不重试；且只 `head -c 4000`。
  （`/mpp/mipi` 依然**绝对不碰**。）
- **维护标记由 `trap EXIT` 保证删除**，即使脚本中途被打断。

## 结果

### 本次确定下来的

| 结论 | 依据 |
| --- | --- |
| 本板 `work_mode == TDM_OFFLINE` | `reset!!!` 打印是 OFFLINE-only，两跳源码证明 |
| `vin_tdm.c:1095` 静默返回**不是**本板根因 | 上一行条件不成立 |
| DT 缺 `work_mode` 安全落到 OFFLINE | `0xff` + `clamp_t(0xff,0,1) == 1` |
| `speed_dn_en` 全树只有两处写点，都是私有 ioctl | `vin_video.c:3065` / `:5124` |
| BSP 参考程序 `csi_test_mplane.c` 也不调该 ioctl | 已 grep 确认 |
| `__sunxi_tdm_reset_v2_handle` 有解锁无锁 | `graph_mutex` 全文件唯一命中 |
| 同函数内层循环覆盖外层 `i` | awk 区间输出 |

### 仍然开放

1. **零帧根因仍未定位。** ONLINE 理论关掉之后，剩余候选：
   - `combo_csi_init` 的 `if_type == V4L2_MBUS_UNKNOWN` 分支是**空 no-op**
     ⇒ PHY 从未被配置（`if_type` 只在各 `s_*_mbus_config` 里赋值）；
   - `sensor→mipi` 链路使能状态 / `entity.stream_count` 的门控；
   - `tdm_set_rx_data_rate` 的 `#if VIN_FALSE` 死分支 vs `#else` 活分支
     （活分支在 `!speed_dn_en` 时整函数早退，`:776`）；
   - `graph_mutex` 无锁解锁对本板是否真的无害 —— **未知**。
2. **`tools/board_vin_stage_probe.sh` 一次都没在真机上跑过。**
   唯一一次执行是在线时预检，正确地报了「开发板不可达」。
   它的可用性目前只有 `bash -n` 一层保证。
3. **仓库内两份记录互相矛盾，未擅自裁定**：
   [`../pitfalls/journal-log-date-attribution.md`](../pitfalls/journal-log-date-attribution.md)
   断言 TDM 自 11-15 起完全静默；而
   [`2026-11-15-edge-url-cloud-direct-and-capture-race.md`](2026-11-15-edge-url-cloud-direct-and-capture-race.md)
   记录了 11-15 的 06:04 / 06:18 / 06:33 有帧流过。两者必有其一不准，
   需要板端全量 journal 才能判定。**已在此标记，未做取舍。**
4. **判据 B 的两个正向对照在全仓库记录里从未出现过**：
   `grep` 所有记录，既没有 `vb is` / `wh_fps`（可证 OFFLINE + `speed_dn_en==1`），
   也没有 `to close`（可证 `bus_if == CSI2_DPHY` 走过 reset 路径）。
   这意味着此前「`reset!!!` 说明 TDM 确实被配过、确实进过中断」这句话
   是**没有证据支撑的**，本次一并撤回。

### 后续

等开发板在线后，按用户要求**一次性**执行：

```bash
./tools/board_vin_stage_probe.sh --dry-run   # 先预检 + 读 DT work_mode
./tools/board_vin_stage_probe.sh             # 两轮完整采集
```

一次采集就能同时回答三个问题：`work_mode` 真值、TDM 接收通道是否使能、
MIPI 的 `s_mbus_config` 与 `combo_csi_init` 有没有走到。
不需要升格为正式 OpenSpec change —— 这是排查，不是行为变更。
