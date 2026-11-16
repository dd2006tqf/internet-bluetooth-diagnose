# 同一个文件里三个名字相近的 reset 函数，我据一条日志反推出了相反的结论

> 发现于：2026-11-16（纯源码定位，自我纠错）
> 位置：`drivers/vin/vin-tdm/vin_tdm.c` — `:1465` / `:1607` / `:1746` / `:1915` 四个函数
> 影响：把 `work_mode` 判成 `TDM_ONLINE`，于是把一条**本板不会命中**的
> 静默返回路径当成了根因，方向整个反了。如果没纠正，上板验证会白跑一轮。

## 现象

`vin_tdm.c` 里有一条必然可见的打印（`vin_print`，不受 `CONFIG_VIN_LOG` 影响）：

```
__sunxi_tdm_reset_v2_handle:workqueue tdm_rx0 reset!!!, isp frame count is 0
```

我当时的推理是：

1. 这行在 `:1814`；
2. `:1479` 有一句 `if (tdm->work_mode == TDM_OFFLINE) return;`；
3. 所以这条打印只可能出现在 ONLINE 分支；
4. 板端日志里有这行 ⇒ **本板是 TDM_ONLINE**；
5. ONLINE + `speed_dn_en == 0` ⇒ 命中 `:1095` 静默返回 ⇒ 零帧。

**第 2 步是错的。** `:1479` 不属于 `:1814` 所在的函数。

## 成因

### 一、四个函数，两条不同的「OFFLINE 门」

```c
1465: static void __sunxi_tdm_wdr_reset(struct tdm_dev *tdm)
1479:     if (tdm->work_mode == TDM_OFFLINE)
1480:         return;                          /* ← 直接门：OFFLINE 直接退出 */

1571-1585:  vin_print("%s:tdm_rx%d reset!!!\n", ...)      /* 本函数内的打印 */

1607: static void __sunxi_tdm_reset_v2(struct tdm_dev *tdm)
         /* 全树无调用者 —— grep "__sunxi_tdm_reset_v2(" 只命中定义本身 */

1746: static void __sunxi_tdm_reset_v2_handle(struct work_struct *work)
1767:     if (!tdm->tdm_rx_reset[vind->vinc[i]->tdm_rx_sel]) continue;
1814:     vin_print("%s:workqueue tdm_rx%d reset!!!, isp frame count is %d\n", ...)

1915: static void __sunxi_tdm_reset(struct tdm_dev *tdm)
1917: #ifndef TDM_V200                       /* ← 本板定义了 TDM_V200，整个函数被编译掉 */
```

`__sunxi_tdm_reset_v2_handle()`（`:1746`）头部**没有任何 `work_mode` 判断**。
它的门是**间接**的，藏在一个数组里：

```c
/* 停用相与启用相各有一份，:1767 / :1850 */
if (!tdm->tdm_rx_reset[vind->vinc[i]->tdm_rx_sel])
    continue;               /* 数组元素是 0 就什么都不做，包括 :1814 那条打印 */
```

而 `tdm_rx_reset[]` 的全部「置 1」写点**只有一个函数**：

```c
/* :2091-2095 */
static void tdm_rx_set_reset(struct tdm_dev *tdm, unsigned int rx_id)
{
#ifdef TDM_V200
    if (tdm->work_mode == TDM_ONLINE)
        return;             /* ← ONLINE 时永不置位 */
```

（`grep -n "tdm_rx_reset" vin_tdm.c` 的结果里，所有 `= 1` 都落在 `2091-2114`
区间内；其余全是 `= 0` 的清零。）

**所以链条是**：`work_mode == ONLINE` ⇒ `tdm_rx_set_reset()` 早退 ⇒
`tdm_rx_reset[]` 恒为 0 ⇒ 循环体全部 `continue` ⇒ `:1814` 永不执行。

**结论与最初相反：这条打印只可能出现在 `TDM_OFFLINE`。**
板端日志里有它 ⇒ 本板是 OFFLINE ⇒ `:1095` 那条静默返回**不会命中**。

### 二、为什么会看错

三个原因叠加：

| 原因 | 具体 |
| --- | --- |
| **名字** | `__sunxi_tdm_wdr_reset` / `__sunxi_tdm_reset_v2` / `__sunxi_tdm_reset_v2_handle` / `__sunxi_tdm_reset` —— 四个名字共享 `sunxi_tdm_reset` 主干 |
| **同一个文件** | 不需要跨文件，`grep` 一次就能看到 `work_mode == TDM_OFFLINE` 和那条打印同时出现 |
| **门的位置不同** | 一个在函数头（直接 `return`），一个在循环体内（`continue`），而且判断的是**被别的函数写入的数组**，不是 `work_mode` 本身 |

用 `grep` 找「打印点附近有没有 work_mode 判断」时，`:1479` 距离 `:1814`
只有 335 行，中间隔着 `:1607` 一个函数定义 —— 肉眼扫过去很容易当成同一个作用域。

### 三、这份源码里还有两个死/编译掉的 reset

顺带记下来，免得下次再翻：

| 函数 | 状态 | 怎么确认 |
| --- | --- | --- |
| `__sunxi_tdm_reset_v2`（:1607） | **无调用者** | `grep "__sunxi_tdm_reset_v2("` 只命中定义 |
| `__sunxi_tdm_reset`（:1915） | **被编译掉** | 函数体在 `#ifndef TDM_V200` 里，本板定义了 `TDM_V200`（`platform_cfg.h:65`） |
| `__sunxi_tdm_wdr_reset`（:1465） | 活着，唯一调用点 `:2412`（`tdm_isr` 的 `tx_fifo_under` 分支） | |
| `__sunxi_tdm_reset_v2_handle`（:1746） | 活着，`INIT_WORK(&tdm->tdm_reset_task, ...)`（`:2508`） | 由 `tdm_isr` 里 8 处 `schedule_work(&tdm->tdm_reset_task)` 触发 |

也就是说：**`isr` 里的报错路径一共 8 处会 `schedule_work`，但只有真正把
`tdm_rx_reset[]` 置位的那些才走得到打印。** 看到「有 `schedule_work`
但没有 `reset!!!` 打印」不代表 work 没被调度，可能只是守卫全 `continue` 了。

## 修法

判据本身不用改（`reset!!!` 打印确实是 OFFLINE-only，这条结论现在有了
**两跳**源码依据）。要改的是**取证方法**：

**给每一条日志绑死「它所在的那个函数」，而不是「它附近有没有某个 if」。**

具体做法（本次事后补的）：

```bash
# 1. 先定位打印点的绝对行号
grep -n "reset!!!" vin-tdm/vin_tdm.c

# 2. 再定位所有函数的起始行，把打印点归到区间里
grep -n "^static .*(\|^void .*(\|^int .*(" vin-tdm/vin_tdm.c

# 3. 只在该函数的区间内找门（grep -n 的行号落在区间内才算）
```

第 3 步是关键：`awk 'NR>=1746 && NR<=1913'` 之后再 grep `work_mode`，
得到的结果是——**只有一处**，且是末尾的
`if (tdm->work_mode == TDM_ONLINE && need_ve_callback)`（`:1906`），
跟门禁无关。一眼就能看出「这个函数没有直接门」。

## 边界

- 「`reset!!!` 打印 = OFFLINE-only」依赖 `tdm_rx_reset[]` **只在
  `tdm_rx_set_reset()` 里被置 1**。这是对**当前这份源码**穷举
  `grep -n "tdm_rx_reset"` 的结果。驱动升级后必须重查：
  `grep -n "tdm_rx_reset\[[^]]*\] = 1" vin_tdm.c`，若出现 `tdm_rx_set_reset`
  之外的写点，本文结论失效。
- 本仓库的 `bsp-vin/vin_tdm.c` 与板端 `vin_tdm.c` 是否**逐字一致**没有核对过
  （早期只逐字核对了 7 份文件，`vin_tdm.c` 不在其中）。若板端驱动是别的
  分支，行号可能偏移，但「间接门」这个结构大概率仍在。
- 这条经验是**单向**的：`reset!!!` 出现 ⇒ OFFLINE；
  `reset!!!` 不出现 ⇏ ONLINE（可能只是没触发过错误中断）。

## 教训

**`grep` 给的是行号，不是作用域。** 在长文件里靠「A 行和 B 行都命中了同一个
符号」来推断因果关系，等价于假设整个文件是一个函数。正确的做法是把
「函数边界」也 `grep` 出来，先做区间归属，再谈因果。

更一般的说法：**一条日志的语义由「它所在的函数」+「进入该函数的全部守卫」
共同决定**，而不是由它自己那行文字决定。看到一条 `vin_print`，
第一件事应该是问：**要执行到这一行，前面必须有哪些条件成立？**
本案的答案是「`tdm_rx_reset[]` 非 0」，而它又等价于「不是 ONLINE」——
两跳之后结论正好和直觉相反。

（配套的一课：这条纠正来自「把 1746-1913 整段读一遍」，
而不是来自任何新的外部信息。同一份源码，读法不同，结论相反。）
