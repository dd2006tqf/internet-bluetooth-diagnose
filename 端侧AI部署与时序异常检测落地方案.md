# 端侧 AI 部署、时序异常检测与板级底层扩展落地方案

> 本文系统记录在 Radxa Cubie A7A（Allwinner A733 / sun60iw2）硬件平台上的三类落地方案：
> （A）端侧时序异常检测的 NPU 原生部署与既有因果引擎闭环；
> （B）内核驱动与设备树层的"硬件环境感知"扩展；
> （C）V4L2 摄像头多模态融合的可选路径。
> 文档同步维护"已验证事实 / 进行中 / 未启动"三态，避免规划话术与工程事实混淆。

---

## 零、2026-10-13 实测确认：板级现状快照

以下为在开发板上逐条命令实测的当前状态，是后续所有规划的事实基线：

| 项 | 实测结果 |
|---|---|
| SoC / 内核 | Allwinner A733 `sun60iw2`，内核 `5.15.147-99btf1-a733`（SMP PREEMPT aarch64） |
| llama-server | **运行中**：`llama-server.service` active，`/home/radxa/llama/bin/llama-server -m qwen2.5-0.5b-instruct-q4_k_m.gguf --host 127.0.0.1 --port 8080 -c 1024 -t 4`，内存 582MB，`/v1/chat/completions` 实测正常返回 |
| NPU | `vipcore.ko` 已加载，`/dev/vipcore` 存在；VIPLite 运行时在 `/home/radxa/npu_test/mobilenetv2_demo_linux_a733/`（`libVIPhal.so`、`libNBGlinker.so`、`mobilenetv2-12_pcq_a733.nb`、可执行 demo） |
| `.nb` 模型 | 板上**仅有官方 MobileNetV2**，无自有时序模型 `.nb` |
| 内核构建环境 | **`/lib/modules/5.15.147-99btf1-a733/build` 完整存在**（Makefile、Module.symvers、scripts、include），`linux-headers-5.15.147-99btf1-a733` 在 `/usr/src/`——**可在板上直接 `make` out-of-tree `.ko`**，gcc 10.2.1 可用 |
| Overlay 源码 | `radxa-overlays-0.2.18` 包在 `/usr/src/`，`/boot/dtbo/` 有 camera-13m-214/4k-415/8m-219、display-10fhd/8hd、twi2/twi7、uart2/3/4、spi1/spi3-spidev、pwm1-* 等 overlay（当前全部 `.disabled`） |
| GPIO | `gpiochip0` 352 线（SoC pinctrl）；`gpiochip1` 64 线为 **40pin 排针 GPIO**（可见 `PIN_13`/`PIN_15`/`PIN_22`、`wifi_power_en`、`wifi_chip_en`、`usb0-vbus` 等命名） |
| I2C | `/dev/i2c-{0,13,14,15,20}` 五条总线；i2c-20 上 0x30 有设备，i2c-0 多路 `UU`（内核驱动占用，多为 PMIC 类） |
| 引导 | U-Boot + extlinux（`/boot/extlinux/extlinux.conf`），`uEnv.txt`/`hw_intfc.conf` 已废弃，改用 `rsetup` 配 overlay、`/etc/kernel/cmdline` + `u-boot-update` 改 bootargs |
| 存储 | 当前运行于 sda SSD（120GB），mtdblock0 8MB 为 SPI NOR（bootloader） |
| 开发板连接 | 本机经 `ssh board`（127.0.0.1:2222 反向隧道）访问，`cloud-tunnel.service` 自动重连 |

**关键修正**：本项目端侧 AI 是"两条腿"——①**Qwen2.5-0.5B 解释层已部署、已运行、已接入 D-Bus `GetDiagnosis` 链路**（真实完成）；②**NPU 时序异常检测仅验证到官方 MobileNet demo，自有模型的 `.nb` 转换与 VIPLite 推理程序尚未实现**。简历上"3 TOPS NPU 毫秒级原生部署"目前只由官方 demo 支撑，不是已完成事实。

---

## 一、端侧时序异常检测（既定主线，最高优先级）

---

## 一、系统全景架构与定位

整套系统并非“用概率大模型替换传统诊断代码”，而是构建**三层职责清晰、单向解耦的端侧可信诊断闭环**：

```text
 ┌────────────────────────────────────────────────────────┐
 │ 1. 上游传感器与特征层 (eBPF 探针 + 3 TOPS NPU 时序模型) │
 │   • eBPF / netlink 采集 5 维核心指标流 (5s 采样周期)     │
 │   • NPU 运行 6×5 滑动窗口时序模型 (30秒微观特征窗口)    │
 │   • 职责: 捕获多指标微弱协同恶化信号，毫秒级输出结构化事件│
 └───────────────────────────┬────────────────────────────┘
                             │ 结构化异常事件 (Anomaly Event + Confidence)
                             ▼
 ┌────────────────────────────────────────────────────────┐
 │ 2. 中枢真值判决层 (确定性因果规则引擎 - C++ 核心服务)    │
 │   • 9 路 SLE 评估器 (Core / Service / Advisory 三层聚合) │
 │   • 证据质量门 (防观测丢失误判) + 因果抑制图剪枝        │
 │   • 职责: 绝对真值把关，微秒级输出确定性诊断事实与根因   │
 └───────────────────────────┬────────────────────────────┘
                             │ 权威事实快照 (DiagnosisFacts)
                             ▼
 ┌────────────────────────────────────────────────────────┐
 │ 3. 下游交互呈现层 (板端 0.5B 本地小模型 SLM)            │
 │   • 仅作为语言解释器，输入事实与规则先验，输出人话摘要   │
 │   • 契约校验 (Contract Validator) + 校验失败回退规则模板 │
 │   • 职责: 零幻觉风险的人机交互呈现                       │
 └────────────────────────────────────────────────────────┘
```

---

## 二、端到端五步实施路线图

### 阶段 1：数据准备与清洗规范（已完成）

- **数据基础**：开发板历时 35 天持续采集，累计 `215,770` 行真实数据，成功导出为基准数据集。
- **清洗与过滤纪律**：
  1. **去重与格式校验**：剔除 `(ts, iface)` 毫秒重复记录；
  2. **未知标签剔除**：过滤 `quality=UNKNOWN` 记录（共 1,680 行，属评估过渡态，不可作为训练标签）；
  3. **哨兵值标记**：识别并剔除 `rtt_ms=-1`, `rssi_dbm=-1000`, `jitter_ms=-1`, `tcp_loss=-1`；
  4. **特征列可用性收敛**：
     - 弃用缺失率达 91.4% 的流量字段（`traffic_bps`, `traffic_pps`）；
     - 将 `tcp_loss_status=stale` 明确界定为“持续无丢包”，按 0 填充；
     - 最终锁定 **5 大高可用核心特征**：`rtt_ms`, `jitter_ms`, `rssi_dbm`, `tcp_loss`, `flows`（各特征有效率均高于 85%）；
  5. **断层时序保护**：检测到全周期内存在 116 处超过 30 秒的采集缺口（单次最大停机缺口 58.9 小时）。按断层切出 113 个有效时序连续段，**严禁跨断层滑动构建时序窗口**；
  6. **防标签泄露红线**：严禁将现有规则派生字段 `score` 传入特征，杜绝模型蜕变为简单规则复刻器。

---

### 阶段 2：电脑本地训练与评估（进行中，注意与 ONNX 导出绑定）

为了规避开发服务器仅存 550MB 内存的 OOM 风险，模型训练与评估在算力充裕的 PC 本地主机执行：

1. **输入表示与时序切分**：
   - 提取 $6 \text{ 个连续时间点} \times 5 \text{ 维特征} = 30 \text{ 维输入向量}$（覆盖 30 秒滑动观测窗口）；
   - 严格按时间序列先后顺序切分（严禁随机打散防止未来数据泄露）：
     - **前 60%**：训练集（学习健康网络下的微观特征协同分布）；
     - **中间 20%**：验证集（确定最佳异常重构误差判定阈值）；
     - **后 20%**：独立测试集（客观验证模型的泛化能力与误报率）。
2. **基线模型对比体系**：
   - **基准 1（统计规则基线）**：基于静态多维度硬阈值的传统判定；
   - **基准 2（时序 AutoEncoder）**：轻量级自编码网络，通过正常样本低重构误差、异常样本高重构误差输出异常置信度；
   - **基准 3（时序 1D-CNN / Transformer）**：评估复杂时序特征建模增益。
3. **关键评估指标**：
   - 必须独立报告 Precision、Recall、F1-Score、ROC-AUC 以及**每小时误报次数（False Alarms/Hour）**；
4. **输出产物**：
   - 对比评测报告；
   - 导出通用浮点标准格式：`network_anomaly_detector.onnx`。

---

### 阶段 3：NPU 交叉编译与 INT8 量化转换

将通用 ONNX 权重转换为全志 A733 平台专用硬件二进制：

- **工具链环境**：使用全志官方 `ACUITY / Pegasus` 模型转换工具容器（`ubuntu-npu` 镜像）；
- **转换流水线**：
  $$\text{network_anomaly_detector.onnx} \xrightarrow{\text{Pegasus Import}} \text{Acuity Model} \xrightarrow{\text{Quantization INT8}} \text{PCQ 量化模型} \xrightarrow{\text{A733 目标编译}} \text{network_anomaly_a733.nb}$$
- **量化校准**：选取验证集中 1000 个纯正常特征窗口作为校准样本；
- **产物优势**：量化为 INT8 后模型大小仅几十 KB，推理内存占用趋近于零。

---

### 阶段 4：开发板 C++ 原生 VIPLite 推理落地（**未完成——当前最大缺口**）

> 2026-10-13 实测确认：本阶段目前**仅完成"官方 MobileNet demo 能跑通"这一验证**，自定义时序模型的推理程序**尚未编写、尚未集成**。以下是规划中的执行链路，不是已完成事实。

直接在 Radxa Cubie A7A 硬件上链接官方 VIPLite 动态库执行原生推理：

- **板载环境（已验证）**：
  - 核心驱动：`vipcore.ko`，设备节点 `/dev/vipcore`（实测存在）；
  - 动态库依赖：`/home/radxa/npu_test/mobilenetv2_demo_linux_a733/lib/` 下 `libVIPhal.so`, `libNBGlinker.so`（实测存在）；
  - **实测性能**（官方 MobileNet demo）：模型加载 6.75 ms，**单次前向推理 2.15 ms（2151 us）**——此数据来自官方 demo，不能直接套用为时序模型推理耗时，但可作为 NPU 链路可用性的基线证据。
- **待实现的自定义推理程序链路**：
  1. 调用 `vip_init()` 完成 NPU 硬件与内存环境初始化；
  2. 调用 `vip_create_network()` 将**自有** `network_anomaly_a733.nb` 载入 NPU 核心；
  3. 创建固定 30 维输入/输出张量缓冲区；
  4. 采集服务每 5 秒将最新指标压入环形窗口，提取最近 6 点填入张量；
  5. 调用 `vip_run_network()` 前向推理；
  6. 对比输出重构误差，超过验证集门限时触发 `AnomalyEvent`；
  7. 将结构化异常事件（含类型、置信度、特征快照）经 D-Bus 上报或持久化落库，接入 `MonitorManager` 插件生命周期与 `device_events` 事件体系。

**集成形态决策点（待 OpenSpec 规划阶段确定）：**
- 方案 A：编成 `weaknet-dbus-server` 的一个 monitor 插件（in-process，共享环形窗口与事件总线）；
- 方案 B：独立 `weaknet-npu-infer` 进程经 D-Bus 上报（故障域隔离，NPU 崩溃不拖垮主服务）。
- 倾向方案 B 的理由：VIPLite 厂商运行时稳定性未知、`.nb` 加载属重资源操作，独立进程更稳；代价是多一个 D-Bus 边界。

---

### 阶段 5：与既有诊断引擎的业务闭环（LLM 腿已完成，NPU 腿待接）

1. **证据链输入**：
   - NPU 输出的并非不可逆的最终诊断，而是**一路全新的高敏结构化证据（AI Anomaly Evidence）**；
   - 该证据送入 C++ 服务端 `DiagnosisEngine`，与 9 个常规 SLE 评估器并列；
2. **规则把关与因果抑制**：
   - 证据必须经由 `ObserverQuality` 质量门校验（剔除由于探针异常引起的假阳性）；
   - 若出现网络底层断连，经由 `CausalResolver` 因果图自动抑制下游告警，确保根因聚焦；
3. **解释层呈现**：
   - 最终诊断由 DiagnosisFacts 形式送入板端 0.5B 本地小模型进行自然语言说明，契约校验失败自动回退确定性模板，确保系统零故障停机与零幻觉风险。
   - **实测现状（2026-10-13）**：`llama-server` 已随 systemd 常驻运行（Qwen2.5-0.5B Q4_K_M，127.0.0.1:8080，`GetDiagnosis` 调用链通），**解释层这条腿已完成并在线**。缺口全部集中在阶段 3/4 的 NPU 感知层。

---

## 二点五、方向扩展：内核驱动与设备树层的"硬件环境感知"（第二阶段魔改）

> 本节记录 2026-10-13 讨论的"方向 2"落地方案：在现有用户态 eBPF/D-Bus 栈之外，向**内核驱动层**下探，采集物理环境证据补齐 DiagnosisEngine 的因果判别维度。
> 实测已确认板上有完整内核构建环境（`/lib/modules/$(uname -r)/build` + headers + gcc）与 `radxa-overlays` 源码包，可在板上直接编译 out-of-tree `.ko`。

### 2.5.1 定位：不是孤立驱动 demo，而是"环境证据采集层"

产品定位是"工业现场无线信号检测与诊断网关"，只做观测诊断不做控制。这些传感器数据补的是**"无线链路劣化 vs 环境根因"的判别证据**——RSSI 掉 + 机柜振动飙升 + 温度高同时发生时，根因就不是"AP 问题"而是"现场物理环境干扰"。这正好补现有 DiagnosisEngine 缺的物理环境维度。

数据出口链路（与 eBPF 形成"内核编程两条腿"）：

```text
内核态采集（GPIO 中断驱动 / I2C client driver）
    → 用户态边界（sysfs / ioctl / 字符设备）
    → MonitorManager 新 monitor 插件（D-Bus 指标/事件）
    → SQLite 落库 + device_events
    → DiagnosisEngine 环境证据维度
```

### 2.5.2 分档采集计划（按驱动难度与产品价值排序）

| 档位 | 器件 | 可采集数据 | 练到的内核知识点 | 注意 |
|---|---|---|---|---|
| 一档 | SW-420 振动开关 / A3144 霍尔 / PIR | 振动冲击计数、门开合、有人/无人 | GPIO 子系统、`gpio_to_irq`、`request_threaded_irq`、顶/底半部、去抖 | **需自己写 `.ko`**（中断计数无现成驱动），是真正的驱动开发 |
| 二档 | INA219（电流/电压/功率）、SHT30（温湿度）、MPU6050（加速度/角速度） | 电源劣化、温度升高、振动频谱 | `i2c_driver`、regmap、`of_match_table`、设备树匹配 | **内核已有 hwmon/iio 驱动**，接上设备树声明即可用；精力应放在 monitor 插件和因果接入，可改为"读懂并定制 iio 采样格式"而非重写驱动 |
| 三档 | UART→RS485/Modbus 设备、SPI ADC、PWM 风扇转速 | 工业传感器数据、高速模拟量、风扇失效 | `uart_driver`/`spi_driver`/`pwm_chip` | 与 40pin overlay（uart2/3/4、spi1/spi3、pwm1-*）一一对应，进阶项 |

**建议顺序**：先用 GPIO 中断写一个最小内核驱动（排针 PIN_13 等空线），板上直接 `make` + `insmod` 验证编译-加载链路；同时买 INA219 + SHT30（十几元），到前把环境监控 monitor 插件的 OpenSpec change 规划好。

### 2.5.3 工程纪律边界

- 内核 `.ko`、设备树 overlay、U-Boot 配置属**板级环境**，不进 `dist-arm64` 用户态产物；驱动源码可放仓库独立目录、构建脚本化，规划时与 Planner 明确边界。
- 新增 monitor 插件属于**新产品 surface**，必须走 OpenSpec change（不能先写代码再追认）。
- 板上编译 `.ko` 与 `tools/ci.sh` 的用户态交叉编译是**两条独立构建路径**，需在设计文档中写清产物分发方式（`.ko` 是否打包、如何 `insmod`/`depmod`、与 weaknet-server 启动顺序）。

---

## 二点六、方向扩展：V4L2 摄像头多模态（第三期可选）

> 板上 `/boot/dtbo/` 有 camera-13m-214/4k-415/8m-219 三个 overlay（当前禁用），说明 A733 具备 MIPI CSI 能力。购买 Radxa 配套模组后可启用。

**可行链路**：启用 camera overlay → `/dev/video*` → V4L2 subdev 拓扑（Sensor→CSI→ISP）→ `media-ctl` → `v4l2-ctl`/gstreamer 采流 → 可选接 NPU 跑视觉模型（MobileNet demo 已证链路通）。

**唯一站得住的产品结合点**（文档既有口径）：工业视觉质检场景——画面传输卡顿时同时记录 V4L2 buffer 状态、网络 RTT/丢包、NPU 推理耗时，归因"采集端/编码端/网络端"哪一环。原始视频不入 SQLite，存路径或视觉 embedding。

**代价提醒**：V4L2 media controller + ISP + DMA-BUF 零拷贝是深坑；A733 的 sunxi-VIN 驱动栈公开资料少于 RK 生态。**优先级排在 NPU 时序检测与 GPIO/I2C 环境驱动之后**，除非 Camera Pipeline 经历本身是目标。

---

## 三、简历提炼与面试核心话术

> **红线**：以下三条话术描述的是**完成后的目标状态**。截至 2026-10-13 实测，第一条的数据基础成立，第二、三条的 NPU 推理程序尚未实现；写进简历前必须把阶段 3/4 真正做完，否则属夸大。

完成此链路后，端侧 AI 部署经历将形成极具竞争力的技术表达：

- **时序网络异常检测模型**：
  > 针对工业边缘网关，基于 21 万条真实网络生产指标，提取 6×5 滑动窗口构建时序自编码器（AutoEncoder），在严格独立时间测试集上完成对比验证，成功捕获传统硬阈值无法感知的多指标协同劣化微观故障。
- **端侧 3 TOPS NPU 毫秒级原生部署**：
  > 借助 ACUITY 完成模型 INT8 量化并交叉编译为 NPU 专用 NBG 二进制；在 Radxa Cubie A7A（Allwinner A733）上基于 C++ 原生链接 VIPLite 运行时实现嵌入式推理服务，单次时序推理耗时仅 **2.15 ms**（官方 MobileNet demo 实测，时序模型实测值待补），推理内存波动小于 5 MB。
- **端侧三层可信诊断闭环**：
  > 创新性设计”NPU 毫秒级异常感知 $\rightarrow$ 确定性因果规则引擎真值判决 $\rightarrow$ 0.5B 本地小模型语言解释”的三层协同架构；坚持规则持有权威真值判定，模型仅负责敏感检测与人机交互，兼顾了端侧 AI 的高灵敏度与工业系统的零幻觉可信度。

**完成后可追加的第四、五条**（对应第二、三节扩展落地后）：

- **内核态环境证据采集驱动**：
  > 针对工业现场物理环境归因需求，编写 GPIO 中断内核驱动（`request_threaded_irq` 顶/底半部分离 + 去抖）与 I2C 传感器接入，通过设备树 overlay 完成引脚复用与器件声明，经 sysfs 边界将振动/温度/电源劣化证据送入用户态诊断引擎，补齐无线劣化的物理环境判别维度。
- **内核双路径观测栈**：
  > 在同一嵌入式平台并行落地 eBPF 无侵入观测（kprobe/tracepoint/CO-RE）与内核模块主动采集（GPIO 中断 + I2C client driver）两条内核级数据通路，统一经 D-Bus 进入分层 SLE 评价与因果诊断体系。
