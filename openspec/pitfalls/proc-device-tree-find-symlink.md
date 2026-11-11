# `find /proc/device-tree` 永远是空的——它是个符号链接，而 find 默认不跟随

> 发现于：2026-11-11（板端实测）
> 位置：`openspec/tasks/2026-11-08-stage2-overlay-drill-playbook.md` Step 1 / Step 3 / Step 4 的所有判据命令
> 影响：判据在「overlay 未生效」和「已生效」两种状态下输出都是空，**不具备区分能力**

## 现象

手册里所有设备树判据都长这个样子：

```bash
for n in $(find /proc/device-tree -name "uart@2501*" 2>/dev/null); do
  echo "$n = $(tr -d "\0" < "$n/status")"
done
```

板端实测（`find (GNU findutils) 4.8.0`，内核 5.15）：

```text
--- find /proc/device-tree (无斜杠) ---
0
--- find /proc/device-tree/ (有斜杠) ---
9
--- find -L /proc/device-tree ---
9
--- ls /proc/device-tree/ ---
51
```

`ls` 能列出 51 个顶层节点，`find` 却返回 0 个。**同一个路径，两种结果。**

这一条比「节点名猜错」更致命：即便把 `uart@2501*` 改成正确的
`uart@2502*`，输出依然是空。判据永远报告「没找到」，
而「没找到」在演练语境里就等于「overlay 没生效」。

## 成因

```text
$ ls -ld /proc/device-tree
lrwxrwxrwx 1 root root 29 ... /proc/device-tree -> /sys/firmware/devicetree/base
```

`/proc/device-tree` 不是目录，是指向 sysfs 的**符号链接**。

GNU find 的默认行为（不带 `-L` / `-H`）是：**遇到符号链接不跟随**。
它把 `/proc/device-tree` 当成一个「链接文件」来 stat，看它不匹配
`-name "uart@*"`，于是不输出、也不往下走。

用 `-printf "%y"` 直接看 find 眼里它是什么：

```text
$ find /proc/device-tree -maxdepth 0 -printf "%y %p -> %l\n"
l /proc/device-tree -> /sys/firmware/devicetree/base     ← l = 链接

$ find /proc/device-tree/ -maxdepth 0 -printf "%y %p -> %l\n"
d /proc/device-tree/ ->                                   ← d = 目录
```

**末尾多一个斜杠，find 就当成目录了。** 这不是 bug，是 POSIX 的规定：
路径末尾带 `/` 时，最后那个分量必须被解析为目录，因此符号链接会被解引用。

三个能用的写法：

| 写法 | 结果 | 说明 |
| --- | --- | --- |
| `find -L /proc/device-tree ...` | ✅ 9 | 显式跟随所有符号链接 |
| `find -H /proc/device-tree ...` | ✅ 9 | 只跟随**命令行参数**上的链接（更精确） |
| `find /proc/device-tree/ ...` | ✅ 9 | 靠末尾斜杠强制解引用 |
| `find /proc/device-tree ...` | ❌ 0 | 默认不跟随 |

顺带一个容易误判的对照：`ls /proc/device-tree` 和 `ls /proc/device-tree/`
**都能**列出 51 项。因为 `ls` 默认就解引用命令行参数上的符号链接。
所以「`ls` 能看到、`find` 看不到」不是路径坏了，是两个工具默认行为不同。

## 修法

在判据里显式跟随，并顺手把范围从「猜一个节点」放宽到「全量导出再 diff」：

```bash
for n in $(find -L /proc/device-tree -name "uart@*" 2>/dev/null); do
  printf '%s = %s\n' "$(basename "$n")" "$(tr -d '\0' < "$n/status")"
done | sort
```

实测输出（overlay 生效后）：

```text
uart@2500000 = okay      ← console（uart0，救砖通道）
uart@2501000 = disabled
uart@2502000 = okay      ← 被 sun60iw2p1-uart2.dtbo 启用
uart@2503000 = disabled
uart@2504000 = disabled
uart@2505000 = disabled
uart@2506000 = disabled
uart@7080000 = disabled
uart@7081000 = disabled
```

## 边界

`-L` 会跟随**所有**符号链接，不只是开头那个。设备树里通常没有
指向自身祖先的链接，所以这里安全；但如果把 `-L` 用在
`/sys` 这类到处是链接的树里，务必配 `-maxdepth` 或改用 `-H`。

另一个更实际的坑：`/proc/device-tree/**status` 的读取需要 root，
或者至少对 sysfs 有读权限。若用 `sudo` 跑 find 但不用 `sudo` 跑 `tr`，
会得到一堆空字符串而不是报错——判断「status 是 empty」时先确认权限。

## 教训

**一个判据如果在「成功」和「失败」两种状态下输出相同，它就不是判据。**

在把任何命令写进验收流程之前，必须先在**已知正确**的状态下跑一次，
确认它非空、且内容符合预期。本次这条 `find` 命令从写下那天起
就没工作过，但它看起来完全正常——`2>/dev/null` 还把唯一的线索吞掉了。

`2>/dev/null` 在这种诊断命令里是反模式：它让「路径不存在」
「权限不足」「命令没装」全部退化成「没有结果」。
