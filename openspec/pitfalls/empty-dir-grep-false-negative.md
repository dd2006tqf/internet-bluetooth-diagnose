# 搜一个空目录得到 0 命中，把「没有」当成结论

> 发现于：2026-11-19
> 位置：`/usr/src/linux-a733-5.15/`（板端），用 `grep -r e902` 判定厂商 BSP 是否支持 E902
> 影响：得出「全树无 e902，全志没有支持」的**反向结论**，几乎据此否决整条技术路线

## 现象

排查 A733 的 E902 RISC-V 协处理器是否被内核支持时，执行：

```bash
sudo grep -rn "e902" /usr/src/linux-a733-5.15/drivers/ \
     /usr/src/linux-a733-5.15/arch/arm64/boot/dts/sunxi/ | head -30
```

输出为空。同一脚本里 `ls /usr/src/linux-a733-5.15/drivers/remoteproc/` 也是空，
`grep -A20 "sunxi_rproc_match" ...` 也是空，`head -5 .../Makefile` 也是空。

**所有检查一起返回空**——我当时把它读成「厂商 BSP 完全没有 E902 支持」。

实际上那个目录**顶层有 0 个条目**：

```bash
$ sudo ls -1 /usr/src/linux-a733-5.15/ | wc -l
0
```

它是个**空目录**。搜一个空目录，无论搜什么都是 0 命中。

真正的内核树是隔壁的 `/usr/src/linux-headers-5.15.147-100btf1amp-a733/`，
里面有 `arch/  bsp/  include/  Makefile  Module.symvers  scripts/` 六个条目，
其中 `bsp/include/` 才是有价值的 BSP 头文件。

## 成因

三重因素叠加：

**1. 目录名极具误导性。** `linux-a733-5.15` 看起来就是「A733 的 5.15 内核源码」，
而 `linux-headers-5.15.147-100btf1amp-a733` 看起来只是「头文件包」。直觉会
去搜前者、跳过后者。实际上**恰恰相反**：前者是空的残留，后者带着 `bsp/`。

**2. 空目录的 grep 返回值与「搜过了、真没有」完全一样。** 两者都是空输出、
都是 `rc=1`。grep 不会告诉你「目标里一个文件都没有」，它只是没有东西可匹配。

**3. 我在同一个脚本里做了多项检查，所有项一起返回空。** 这反而增强了
「确实没有」的可信感——「四种方法都说是空的，显然是真的没有」。但真相是
**四个检查共享同一个坏前提**，它们不是四个独立证据，是同一个错误的四次回声。
这正是 `openspec/pitfalls/README.md` 里
「判据的两个操作数来自同一个不信任的来源」那条的变体。

## 修法

**任何 `grep -r` 之前，先确认目标非空。**

```bash
# 判据前置检查：目标存在且非空
K=/usr/src/linux-headers-5.15.147-100btf1amp-a733
[ -d "$K" ] || { echo "目录不存在"; exit 1; }
n=$(sudo find "$K" -maxdepth 1 -mindepth 1 | wc -l)
[ "$n" -gt 0 ] || { echo "目录为空，搜索无意义"; exit 1; }

sudo grep -rn "e902" "$K/bsp/" "$K/arch/"
```

更好的是**先做阳性对照**：用一个已知必然存在的字符串搜一次。

```bash
# 阳性对照：如果这个都搜不到，说明搜索本身有问题
sudo grep -rl "Linux" "$K/Makefile" || echo "!! 阳性对照失败，搜索链路有问题"
```

修正后同一棵树的结果完全不同：

| 关键词 | 空目录（错） | 真实树（对） |
| --- | --- | --- |
| `e902` | 0 命中 | **2 个文件命中**（`sun65iw1-r-ccu.h` 的时钟/复位定义） |
| `remoteproc` | 0 命中 | 2 个文件（`sunxi_remoteproc.h`、`rpbuf.h`） |
| `rpmsg` | 0 命中 | 1 个文件（`aw_rpmsg.h`） |
| `rpbuf` | 0 命中 | 4 个文件 |

## 边界

- 这个坑与 `kprobe-zero-count-false-negative` 是同一族（**假否定**），
  但成因不同：那篇是探针没装进去，本篇是**搜索目标本身为空**。
  两者共同点是「失败形态与真实的零完全相同」。
- `find` 默认不跟随符号链接，也会制造同类假否定
  （见 `proc-device-tree-find-symlink`）。
- **无法自动发现**：没有任何工具会在你搜空目录时提醒你。只能靠流程
  ——凡是要用「搜不到」下结论，先证明搜索面非空。

## 教训

**「我搜了，没有」和「那里什么都没有，所以搜等于没搜」在输出上完全一致。**

用一个否定式结论（「不支持」「不存在」「从未发生」）之前，必须先证明
**观测面本身有内容**。空目录、空结果集、未挂载的路径、没装进去的探针、
被编译掉的日志宏——它们全都长得像「真的没有」。

具体做法只有两条，成本都极低：

1. 搜之前 `ls | wc -l` 或 `find -mindepth 1`，确认目标非空；
2. 搜之前拿一个**已知必然存在**的字符串做阳性对照。

## 关联

- [[kprobe-zero-count-false-negative]] — 同族：假否定，成因是探针没装进去
- [[proc-device-tree-find-symlink]] — 同族：`find` 不跟随符号链接导致空结果
- [[dmesg-not-history-and-nonmonotonic-counters]] — 同族：把可自愈故障读成「从未发生」
- [[vin-log-compiled-out]] — 同族：观测通道整个是空的而不自知
- `openspec/tasks/2026-11-19-a733-e902-bsp-support-verdict.md` — 本次结论的完整证据
