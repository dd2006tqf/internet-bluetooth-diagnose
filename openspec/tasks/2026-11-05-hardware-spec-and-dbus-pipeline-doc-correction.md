# 硬件规格口径订正与 D-Bus 信号管线文档同步

> 日期：2026-11-05
> 分类：工作流外实施
> 结果：已完成

## 构思

**要解决什么问题**：用户准备面试，需要确认简历里 D-Bus 那条分点还成不成立，并决定硬件平台怎么写。核对过程中暴露两类仓库缺陷：

1. **硬件规格自相矛盾**：CPU 拓扑在同一份文档内前后打架（`docs/本地LLM部署方案.md` 第 5 行说 2×A76+6×A55，第 313 行又说"实测 8×A55 无 A76，以实测为准"）；内存型号一处 LPDDR4 一处 LPDDR5；资源数字有两套（CPU<3%/RSS<40MB 与 CPU<1%/30MB）且都无数据支撑。
2. **知识库仍在讲解已删除的代码**：`弱网项目简历知识库.md` 有整节"背景知识 D：信号发送的重试与串行化"，描述 `sendSignalInternal` 的 3 次重试、`send_mutex_` 串行化、`emitChanged` 等——这些代码已于 2026-10-19（commit `8c8e895`）整段删除。用户若按知识库准备面试，等于在讲不存在的实现。

**为什么不做进 OpenSpec change**：只改文档与注释，无产品行为、接口、构建产物或运行时表面变化。走 change 流程会引入 fingerprint 与 surface 契约，但没有可验证的行为增量。

**关键判断**：先实测，再改文档。**不允许按记忆或旧文档推断硬件规格**——这正是矛盾的来源。

## 实现方式

### 实测取证（板端，2026-11-05）

```bash
ssh board 'grep -E "^(processor|CPU part)" /proc/cpuinfo | paste - -'
ssh board 'for c in 0 5 6 7; do echo -n "cpu$c: "; cat /sys/devices/system/cpu/cpu$c/cpu_capacity; done'
```

结果：

| 核 | `CPU part` | 型号 | 最高频率 | `cpu_capacity` |
| :--- | :--- | :--- | :--- | :--- |
| cpu0-5 | `0xd05` | Cortex-A55 | 1.79 GHz | 385 |
| cpu6-7 | `0xd0b` | Cortex-A76 | 2.00 GHz | 1024 |

**结论：本板是 2×A76 + 6×A55 big.LITTLE。** 文档里"实测为 8×A55"的那条"纠正"才是误判。

**误判根因**（值得记下来）：`lscpu` 的 `Model name` 只报 cpu0 的型号，`scaling_available_frequencies` 也只读 cpu0 的频率表。据此推断全核型号，必然得出"全 A55"。**必须逐核读 `CPU part`（或 `midr_el1`）才能分辨大小核。**

其衍生错误：`CPUAffinity=0 1` 绑的是两颗 **A55 小核**，不是 A76。旧文档因误判"全核皆 A55"而认为绑哪两颗都一样。

### 资源实测

```bash
# RSS
awk '/VmRSS/{printf "%.1f MiB\n", $2/1024}' /proc/<MainPID>/status
# CPU（30 秒窗口，累加所有线程的 utime+stime）
```

- **RSS：26.9 MiB**
- **CPU：4.57%（占单核）/ 整机 1.13%**

**必须标注的边界**：这是生产实况单点采样，**不是有/无探针的 A/B 对照**。`tools/analyze_soak.py` 的冻结判据里不含 CPU 项（`grep -c cpu` = 0），`tools/soak_monitor.py` 虽采样 `cpu_percent` 但无对照实验。因此**不能据此宣称"探针开销低"**。

### 修改的文件

| 文件 | 改动 |
| :--- | :--- |
| `docs/本地LLM部署方案.md` | 头部 SoC 拓扑段改为逐核实测表述并写明误判根因；1.1 节表格补核编号与实测内存；架构图去掉 LPDDR5；313 行"8×A55 纠正"改为正确口径并标出 `CPUAffinity=0 1` 绑的是小核；`-t 4` 与线程分配段说明当前 unit 与设计不自洽；排查清单改为指向 cpu6-7 |
| `docs/架构设计-模块与代码结构.md` | §12.4 硬件拓扑补核编号、去掉未证实的 LPDDR5；板端实测注改写 |
| `tools/llama-server.service` | 头部注释改写：拓扑实测值 + 绑的是小核 + 待定项（是否改 `CPUAffinity=6 7`） |
| `弱网项目简历知识库.md` | CPU 行、内存行改为实测值（内存型号标注"未暴露，勿写 LPDDR4/5"）；删掉"内存充裕…硬件底气"；新增"0. 简历上怎么写硬件平台"；资源数字改为实测并标注 A/B 对照缺失；探针数 10/11 → 12；线程数 13 → 12（源文件数）；方法数 29 → 33；**背景知识 D 整节重写**（从"重试发送实现"改为"管线已删除及其决策依据"）；事件管理器章节去掉信号双通道；Q3-Q8 问答重写；两处 30 秒模板、两处简历标准写法同步 |
| `openspec/tasks/2026-11-01-ota-convergence-and-orphan-interface-fixes.md` | 该文件里"CPU 是 8×A55"的结论加删除线并标注已推翻，附正确实测值 |

### 验证

```bash
grep -n "8× Cortex-A55\|8 核 Cortex-A55\|全核皆 A55" --include=*.md .   # 仅剩已标注为"误判"的说明
grep -c "dbus_message_is_method_call" server/src/dbus_service.cpp        # 33
grep -rln "std::thread" server/src/*.cpp | wc -l                         # 12
ls server/src/*.bpf.c server/src/bpf/*.bpf.c | wc -l                     # 12
grep -rn "dbus_message_new_signal" server/src/                           # 源码零命中，仅注释
```

## 结果

### 已完成

- `docs/本地LLM部署方案.md`、`docs/架构设计-模块与代码结构.md`、`tools/llama-server.service`、`弱网项目简历知识库.md` 的硬件口径统一为逐核实测值。
- 知识库中所有描述已删除信号管线的段落重写完毕。
- 用户决策记录：**简历只写 SoC（全志 A733），不写开发板型号**——同一 SoC 会被多家板厂做成不同板，写板名没有信息增量。

### 遗留问题（未修，需后续决策）

1. **`CPUAffinity=0 1` 与设计意图不一致**。该 unit 把推理钉在两颗 A55 小核上，而 `-t 4` 的线程分配注释写的是"2×A76 主力 + 2×A55 辅助"。改绑 `CPUAffinity=6 7` 是显然的候选，但**未实测改绑对推理吞吐与弱网服务的影响**，故本次只标注不改值。
2. **内存型号仍未知**。板端 `dmidecode` 无输出，设备树只给 `reg`，`dmesg` 被清空。文档已改为"未暴露"，但真实型号需查原理图或厂商规格书。
3. **资源数字缺 A/B 对照**。要主张"探针开销低"，需做挂载/不挂载两组的长稳对照。目前只有单点采样。
4. **`弱网项目简历知识库.md` 被 `.gitignore` 忽略**（`.gitignore:106`），本次改动只落盘不入库。这是既有约定，不是本次引入。

**是否升格为正式 change**：不需要，纯文档订正。

### 教训

**推断数据源的覆盖范围必须验证。** `lscpu` 看起来是权威工具，但它只汇总 cpu0；用它推断异构 SoC 的拓扑会系统性出错。同类陷阱：`scaling_available_frequencies`、`cpuinfo_max_freq` 都是 per-cpu 文件，读哪一个只代表哪一个核。
