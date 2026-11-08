# 移除 `weaknet_lradc_keys` 驱动源码

> 日期：2026-11-08
> 分类：工作流外实施（孤儿表面清理）
> 结果：已完成

## 构思

### 要解决什么问题

`kmod/weaknet_lradc_keys.c`（1362 行，Allwinner A733 LRADC 按键驱动）自 2026-11-06
引入以来，一直被定位为「驱动技术栈练习」。2026-11-07 `tools/ci.sh` 改为把整个
`kmod/` 交叉编译进发布包后，这个模块开始产生三项实际成本：

| 成本 | 事实 |
| --- | --- |
| 随包分发却从不加载 | `dist-arm64/kmod/weaknet_lradc_keys.ko`（489 KB）→ `releases/<ver>/kmod/` → `manifest.sha256`。`tools/weaknet-kmod.service` 只 `insmod weaknet_cpu_stat.ko`，对本模块只有 `rmmod` |
| 拖累生产模块 | `tools/kmod_cross_build.sh:147` 的 vermagic 校验对**任一** `.ko` 失败即 `fail`，`ci.sh:186` 随后 `rm -rf dist-arm64/kmod`，连带把真正在用的 `weaknet_cpu_stat.ko` 一起弄丢，`host_pressure` 归因变成 `unavailable` |
| 永不可能工作 | 板端实测 `# CONFIG_AW_LRADC is not set`、`/proc/interrupts` 无 lradc 行、`sunxi-gpadc ... warn: keyadc not select`、DT 里 lradc 节点无任何模拟前端属性。A733 的 LRADC 按键职能已被 gpadc 取代 |

一个永远不加载、永远不可能工作的模块，却有能力让生产模块整批失败——这是负资产。

### 触发这次复核的两次对话

1. **业务价值复核**：产品定位 v1.2 承认了「现场交互终端」，原判定「无人值守、
   没人按键」的前提失效，需要重新评估 LRADC 是否有了业务位置。
2. **输入路径复核**：结论是「仍然没有」。现场终端四类只读交互全部由
   `qt_dashboard` 的点击承接（`test_main_window` 8 条交互测试已绿）；未来若接
   实体键，正确路径是 in-tree `gpio-keys` / `sun4i-lradc-keys` + Qt evdev 插件
   → `QShortcut`，全链路零行自研驱动代码。物理键唯一的结构性优势是「不依赖
   显示通路」，而显示坏了看板就看不见，也就没人会去按那个键。

### 为什么不做成 OpenSpec change

`CLAUDE.md`「规划与实现分离」明确：代码已实现时不套用 OpenSpec change 流程追认。
本次是**删除**既有代码、无行为变更、无生产表面新增，属工作流外实施。按
`CLAUDE.md`「工作流外任务必须留记录」落本文件。

### 备选方案与取舍

| 方案 | 取舍 |
| --- | --- |
| A. 保留源码，仅从 Makefile 移除 | 半途。构建风险消除了，但 1362 行无消费者的孤儿代码还留在 `kmod/` 里，且 README/方案文档仍需解释「为什么它在但没编」 |
| **B. 删除源码 + 从构建移除 + 文档保留方法论（采用）** | ✅ 一次性解决构建风险与孤儿表面；根因定位、四条实测事实、`raw`/`inject`/`selftest` 诊断方法学全部保留在 `kmod/README.md` 第八节，历史实现可 `git show 289cad4` 取回 |
| C. 改写成 GPADC 驱动 | 「驱动面方案」§13.2 已否决：通道被 `sunxi-gpadc` 全占、要与在树驱动仲裁、无业务需求 |
| D. 保留并接入实体按键业务 | 「驱动面方案」§13.3 复议后否决：白名单封闭，物理键没有触摸做不到的事 |

选择 B。

## 实现方式

### 改了哪些文件

| 文件 | 改动 |
| --- | --- |
| `kmod/weaknet_lradc_keys.c` | **删除**（1362 行，`git rm`） |
| `kmod/Makefile` | 去掉 `obj-m += weaknet_lradc_keys.o`，留注释说明缘由与取回方式 |
| `kmod/README.md` | 头部「三个模块」→「两个模块」+ 移除说明；第一节表格删该行；第七节两条历史边界改为脚注；第八节标题改「已移除」，补删除原因、业务价值 v1.2 复核、去掉指向源码的表述；「保留」改「删除源码」 |
| `kmod/hid_multitouch/Makefile` | 「三个模块」→「两个模块」 |
| `kmod/hid_multitouch/README.md` | 同上 |
| `tools/kmod_cross_build.sh` | 头部注释去掉 `weaknet_lradc_keys` |
| `驱动面能力引入与业务融合方案.md` | §13.1 结论改为「已删除」；§13.3 补 v1.2 复核（前提失效但结论不变）；§13.4 改为「当年的价值」；§13.5 动作表改为已执行；§7.6.2 / §10.2 两处 LRADC 对比句、§14 速查表两行、§15.3 不做事项一行同步 |

### 关键决策

**为什么保留 README 第八节的完整内容**（约 150 行）而不是删掉整节。

那节里目前唯一完整记录了「怎么一步步排除硬件故障」：

- 16 组 `CTRL` 寄存器组合扫描（`CHAN_SELECT` × `SAMPLE_RATE` × `KEY_MODE_SEL`）
- `DATA0 = 0x3f` 的真实含义（模拟输入未连接、浮到 VREF 满量程），不是转换失败
- 同一块板上 LRADC 死 / GPADC 活的对照，用来排除「我们驱动写错了」
- `raw` / `inject` / `selftest` 三件套解决的工程问题：驱动注册成功但硬件不动，
  怎么区分软件 bug 与硬件不存在

这是调试方法学，与源码是否在仓库里无关。删掉整节会把最有价值的部分扔掉。

**为什么业务价值判定要重写而不是保留原文。**

原文写「部署形态是无人值守的机柜侧设备，没有用户会站在机柜前按设备上的键」。
这条前提在定位 v1.2（2026-11-08 之前）已被推翻。直接留着会变成一句自相矛盾的话，
后人对不上账。改法是：承认前提失效，改用「输入需求由 `qt_dashboard` 承接 +
未来实体键走 in-tree 驱动」这两条新理由。结论不变，依据换了。

### 验证手段

```bash
# 1. 删除后 kmod 仍可完整交叉编译（不碰真机）
./tools/kmod_cross_build.sh --out /tmp/kmod-verify
# → [kmod]   weaknet_cpu_stat.ko  vermagic 校验通过
# → [kmod]   weaknet_softirq.ko  vermagic 校验通过
# → [kmod] 完成：2 个模块 → /tmp/kmod-verify

# 2. 确认发布链不再产出该 .ko
rm -f dist-arm64/kmod/weaknet_lradc_keys.ko
ls -1 dist-arm64/kmod/            # → 只剩 cpu_stat 与 softirq

# 3. 确认 harness 不管理这些路径（不会引起 fingerprint 变化）
node -e 'const m=JSON.parse(require("fs").readFileSync(".ai-harness/manifest.json"));
         console.log(m.managed_paths.filter(e=>/kmod|tools\//.test(e.path)).length)'
# → 0

# 4. 全仓库残留引用核查（排除 .git / 构建目录 / 历史任务记录）
grep -rn -i "lradc" . --exclude-dir=.git --exclude-dir=build-x86 \
  --exclude-dir=build-arm64-native --exclude-dir=build-qt-arm64 --exclude="*.ko"
```

实测结果：

| 检查 | 结果 |
| --- | --- |
| 交叉编译 | 2 个模块全部 vermagic 校验通过，无 lradc |
| `dist-arm64/kmod/` | 只剩 `weaknet_cpu_stat.ko` / `weaknet_softirq.ko` |
| harness managed_paths | kmod 与 tools 命中 0 条，不影响 fingerprint |
| 残留引用 | 只剩文档说明性提及 + `openspec/tasks/` 历史记录（后者按规矩不改写） |

## 结果

### 最终状态

`kmod/` 现在有两个模块，都是数据采集类：

| 模块 | 状态 |
| --- | --- |
| `weaknet_cpu_stat` | 生产链路（`weaknet-kmod.service` 加载，`host_pressure` 消费） |
| `weaknet_softirq` | 实验代码，随包分发但不加载，路径明确（接 `observer_attribution` 细分） |

`weaknet_lradc_keys` 的构成风险消除：`kmod_cross_build.sh` 的 vermagic 校验
不会再因为一个永不加载的模块而让生产模块整批失败。

### 遗留问题

1. **`weaknet_softirq` 的同类风险仍在**。它有 `.ko`、有构建链，但没有消费者、
   没有 systemd 单元加载，且与 `weaknet_cpu_stat` 共用同一个 `all-or-nothing`
   的构建失败语义。与 lradc 的区别是它**有明确业务路径**（`NET_RX` 软中断饱和
   → `observer_attribution` 第二级细分），所以本轮不动。若长期不接线，应考虑
   同样处理。
2. **实体按键的硬件需求未定**。若将来确实要在机柜面板放一个物理键，正确路径是
   in-tree `gpio-keys` + Qt evdev 插件，不是复活本驱动。届时应按「工作流外实施」
   或 OpenSpec change 重新评估，不要凭「以前有个 1362 行的驱动」就直接复用。
3. **`kmod/README.md` 第八节的细节已无源码可对照**。文中保留了寄存器布局、
   四层结构、实测数据，但读者无法在仓库里找到对应代码（需 `git show 289cad4`）。
   这是有意取舍：保留方法论、放弃可执行性。

### 后续是否值得升格为正式 OpenSpec change

不需要。纯删除 + 文档同步，无行为变更。但**若将来要把 `weaknet_softirq` 接进
`observer_attribution`**，那是一次真实的观测能力扩展（新增归因枚举值、
可能新增 `NetworkExperience` 字段），届时应走 OpenSpec change 或至少产出独立
的定位/契约记录。

## 相关

- `kmod/README.md` 第八节（根因定位与诊断方法学的唯一现存载体）
- `驱动面能力引入与业务融合方案.md` §13（处置方案）、§7.6.2 / §10.2（触摸对比）
- `openspec/tasks/2026-11-06-lradc-keys-driver.md`（引入时的完整记录，未改动）
- `openspec/tasks/2026-11-07-kmod-production-online.md`（kmod 进发布链的记录）
- `docs/产品定位-工业无线诊断网关.md` 第三节之二（现场交互终端封闭白名单）
- `qt_dashboard/ui/main_window.cpp`（白名单交互点的真实实现）
