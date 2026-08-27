# AI 边缘部署与模型训练计划

> 本文记录项目在 Radxa Cubie A7A 上进行端侧 AI 部署、真实网络数据采集、模型训练以及后续 RAG 集成的现状与计划。
>
> **重要说明**：本文区分“已在设备上验证过的事实”“已实现但仅用于流程验证的实验代码”和“尚未完成的目标”，避免把概念验证误认为生产能力。

## 1. 项目目标

将现有的 eBPF 网络监控系统扩展为端侧智能网络诊断系统：

```text
网络监控数据
    ↓
真实数据持久化
    ↓
端侧模型推理（优先使用 NPU）
    ↓
异常检测 / 异常分类 / 趋势预测
    ↓
结构化异常事件
    ↓
RAG 检索网络知识和历史案例
    ↓
根因分析与诊断建议
```

最终目标不是单纯输出一个网络评分，而是回答：

- 当前是否发生异常？
- 异常属于哪一类？
- 哪些指标支持这一判断？
- 历史上是否出现过相似情况？
- 最可能的根因和处理建议是什么？

## 2. 当前项目基础

### 2.1 现有网络监控系统

项目已有 C++ 服务端、D-Bus 客户端 API、eBPF 监控程序和 SQLite 历史数据模块。主要采集能力包括：

- 网络接口状态
- RTT
- Jitter
- Wi-Fi RSSI
- TCP 重传/丢包率
- 流量 BPS
- 包速率 PPS
- 活跃连接数
- 网络质量评分
- DNS、HTTP 延迟和进程网络画像（主要用于实时监控；尚未全部写入历史表）

### 2.2 当前 SQLite 历史表

表名：`network_history`

当前字段：

```text
id
 ts
iface
rtt_ms
jitter_ms
rssi_dbm
tcp_loss
quality
score
traffic_bps
traffic_pps
flows
```

其中：

- `rtt_ms`、`jitter_ms`、`rssi_dbm`、`tcp_loss`、`traffic_bps`、`traffic_pps`、`flows` 是原始或近原始观测值。
- `score` 和 `quality` 是现有规则评分系统产生的派生结果。
- `score` 不应作为正式模型的主要输入，否则模型可能只是复制规则系统。
- `quality` 可用于初期基线标签，但它不是人工确认的故障标签。

### 2.3 真实数据采集

曾经在开发板上以 5 秒间隔采集过超过 5 万条真实数据，并验证过：

- 有效 RTT 约 99% 以上
- 有效 RSSI 约 99% 以上
- 数据包含 GOOD、FAIR、POOR、BAD 等不同网络状态
- 存在真实的 RTT 波动和短时异常

但旧数据库位于 `/tmp/weaknet/history.db`，开发板重启后数据丢失。此前导出的 CSV 也被作为临时训练文件清理。因此，旧的 5 万条数据不能假定仍然存在，重新训练前必须重新确认并导出当前数据库。

## 3. 开发板硬件和 NPU 状态

设备：**Radxa Cubie A7A**

| 项目 | 已确认情况 |
|---|---|
| SoC | Allwinner A733 / `sun60iw2` |
| CPU | 8 核 Cortex-A55，约 2 GHz |
| GPU | PowerVR BXM-4-64 MC1 |
| NPU | Vivante VIP9000，约 3 TOPS |
| NPU 内核驱动 | `vipcore.ko` |
| NPU 设备节点 | `/dev/vipcore` |
| NPU 频率 | 可见 492/852/1008 MHz，曾运行在 1008 MHz |
| 系统 | Debian 11，5.15 Radxa A733 内核 |

### 3.1 已验证的 NPU 用户态环境

从 A733 Model Zoo/官方 demo 中已经部署并运行过：

```text
libVIPhal.so
libNBGlinker.so
mobilenetv2_demo_a733
mobilenetv2-12_pcq_a733.nb
```

官方 MobileNetV2 `.nb` 已成功在板端运行，VIPLite 版本曾显示：

```text
2.0.3.2-AW-2024-08-30
```

这证明了：

```text
A733 NPU 硬件
    + vipcore 内核驱动
    + VIPLite 用户态 runtime
    + 官方 .nb 模型
    + 板端推理 demo
```

整条官方推理链路可用。

### 3.2 ACUITY 转换环境

主机上已经存在：

```text
ubuntu-npu:v2.0.10.2
```

容器中可以找到：

```text
/usr/local/acuity_command_line_tools/pegasus.py
```

Model Zoo 和 A733 脚本也曾在主机下载/解压使用过。A733 模型转换必须使用 A733 对应的优化目标，不要误用 T527 的工具链或目标。

## 4. 已进行的模型实验

### 4.1 AutoEncoder 基线

曾经实现过配置驱动的特征提取和 NumPy AutoEncoder：

```text
6 个时间点 × 5 个指标 = 30 维输入
```

它的作用是验证：

```text
数据 → 特征窗口 → 训练 → ONNX → ACUITY → .nb → NPU 加载
```

但早期实验使用了合成数据，不能作为真实环境效果结论。

### 4.2 Transformer 实验

曾经实现过 NumPy Transformer 和 PyTorch Transformer 实验。后续改为：

- 训练/验证/测试三段划分
- 使用验证集选择阈值
- 使用独立测试集报告结果
- 使用 `WeightedRandomSampler` 和类别权重处理类别不平衡

实验中使用过 `quality` 字段生成标签：

```text
GOOD/FAIR → 正常
POOR/BAD  → 异常
```

这类结果只能说明模型能够拟合现有规则标签，不能证明模型已经发现了规则之外的新故障。此前出现接近 100% 的指标时，必须谨慎解释，不能直接称为产品级性能。

## 5. 正式训练方案

### 5.1 第一版正式输入

正式模型先去掉 `score`，使用原始观测值：

```text
rtt_ms
jitter_ms
rssi_dbm
tcp_loss
traffic_bps
traffic_pps
flows
```

每个采样窗口由连续时间点构成，例如：

```text
窗口长度：6 个时间点
采样间隔：5 秒
窗口覆盖：约 30 秒
```

后续可以比较窗口长度 6、12、24 对效果和延迟的影响。

### 5.2 数据清洗

训练前必须执行：

1. 按 `iface` 和 `ts` 排序
2. 去掉 RTT、RSSI 等关键字段无效的记录
3. 识别 `-1`、`-1000` 等哨兵值
4. 检查重复时间戳和缺失时间段
5. 检查 5 秒采样是否连续
6. 记录清洗前后样本数量
7. 保存数据快照、清洗规则和统计报告

不能把无效值直接当作真实网络异常；应单独记录为 `sample_valid=false` 或从训练集剔除。

### 5.3 时间切分

时间序列不能随机打散后再评价。推荐：

```text
前 60% 时间：训练集
中间 20% 时间：验证集
后 20% 时间：测试集
```

这样可以避免相邻窗口同时出现在训练和测试中造成数据泄露。

### 5.4 标签策略

分三步推进：

#### 第一阶段：规则标签基线

继续使用 `quality` 生成初始标签，但明确标注：

```text
label_source = rule_quality
```

用途是比较模型和现有规则，不宣称是真实故障识别。

#### 第二阶段：人工故障实验标签

在可控环境中记录：

```text
normal
latency_spike
wifi_signal_degradation
congestion
link_down
recovery
dns_slow
```

每个实验保存：

```text
experiment_id
start_time
end_time
scenario
operation
label_source
```

#### 第三阶段：真实业务标签

结合用户感知、业务请求结果、DNS/HTTP 结果和人工确认，形成更可靠的生产标签。

## 6. 模型路线

### 路线 A：异常检测基线

```text
无监督/半监督 AutoEncoder
```

优点：不要求大量人工异常标签。

用途：发现偏离正常基线的网络状态。

### 路线 B：PyTorch Temporal Transformer

```text
原始指标窗口
    ↓
输入投影
    ↓
位置编码
    ↓
Transformer Encoder
    ↓
异常分类或状态预测
```

正式评估要比较：

- 规则评分
- Logistic Regression
- Random Forest/XGBoost
- AutoEncoder
- Transformer

同时报告：

- Precision
- Recall
- F1
- 混淆矩阵
- 每小时误报数
- 推理延迟
- 内存占用

### 路线 C：GNN

GNN 只有在拥有真实图结构后才有意义。未来可建模为：

```text
节点：设备、网卡、AP、网关、DNS、远端服务
边：连接、流量、依赖、路由关系
```

适合的任务：

- 拓扑级故障定位
- 故障影响范围预测
- 多设备协同诊断

当前单设备、单 Wi-Fi 接口数据还不足以支撑有意义的 GNN。不能为了“模型高级”而强行使用 GNN。

## 7. NPU 部署路线

### 7.1 PC 端

训练和模型转换在 x86 主机完成：

```text
真实数据
    ↓
PyTorch 训练
    ↓
固定输入形状 ONNX
    ↓
ACUITY/Pegasus
    ↓
A733 优化目标
    ↓
A733 .nb
```

使用 A733 对应环境：

```text
ubuntu-npu:v2.0.10.x
platform: a733
```

### 7.2 板端

板端只保留：

```text
libVIPhal.so
libNBGlinker.so
模型 .nb
自定义推理可执行文件
```

模型不能直接执行：

```bash
./model.nb
```

必须由 VIPLite/OpenVX 封装程序完成：

1. 初始化 VIPLite
2. 加载 `.nb`
3. 创建输入/输出 tensor
4. 拷贝输入特征
5. 执行推理
6. 读取原始输出
7. 计算异常分数或分类结果
8. 释放资源

官方 MobileNet demo 只能证明 NPU 能加载模型，不能用于解释网络 AutoEncoder/Transformer 输出。

## 8. 与现有 RAG 的集成路线

现有 RAG 代码位于：

```text
AI-assisted analysis/
```

已有组件包括日志捕获、知识库、向量 RAG 和云端模型调用。

推荐将 NPU 输出设计为结构化事件：

```json
{
  "timestamp": "2026-08-27T10:20:00",
  "interface": "wlan0",
  "model_version": "transformer-v1",
  "anomaly": true,
  "anomaly_type": "latency_spike",
  "confidence": 0.94,
  "features": {
    "rtt_ms": 180,
    "jitter_ms": 72,
    "rssi_dbm": -48,
    "tcp_loss": 0.0,
    "traffic_bps": 8200,
    "traffic_pps": 30,
    "flows": 4
  }
}
```

RAG 检索输入应包含：

1. 当前异常类型和置信度
2. 原始指标及变化趋势
3. 相关接口和设备上下文
4. 历史相似异常
5. 网络知识库条目

输出应区分：

```text
模型观察：模型检测到了什么
证据：哪些指标支持判断
历史案例：过去是否出现过
可能原因：按置信度排序
建议动作：用户可以做什么
不确定性：当前无法确认的内容
```

## 9. 多模态阶段

### 9.1 网络 + 系统 + 蓝牙

这是比直接加入摄像头更现实的第一步：

```text
网络指标 + CPU/内存/磁盘 + 蓝牙设备状态
```

这些数据更容易时间对齐，也更贴合网络诊断任务。

### 9.2 网络 + 摄像头

只有在有明确业务场景时才加入摄像头，例如：

- 网络延迟与视频业务质量关联
- 工业设备视觉状态与网络故障关联
- 摄像头检测设备故障后分析网络影响

需要同时保存同一时间窗口的图像/视觉特征和网络数据，不能只把无关联的图像和网络记录拼在一起。

## 10. 当前明确问题和风险

1. **历史数据持久化风险**：数据库曾位于 `/tmp`，必须使用 `/home/radxa/weaknet/data/history.db`。
2. **部署边界风险**：开发板只能保留编译产物，禁止复制源码和在板上直接编译。
3. **D-Bus systemd 风险**：systemd 服务使用用户 session bus 时，开机可能因 bus 不存在而失败，必须设计并验证启动依赖。
4. **标签泄露风险**：`score` 与 `quality` 存在直接规则关系，不能把 score 当正式模型输入并据此宣称智能发现。
5. **数据泄露风险**：随机切分相邻时间窗口会让评估结果虚高。
6. **异常标签风险**：POOR/BAD 是规则标签，不等于人工确认的故障。
7. **NPU 输出风险**：分类 demo 不能直接解释 AutoEncoder/Transformer 输出。
8. **指标缺失风险**：DNS、HTTP 延迟目前没有完整进入历史训练表，根因分析能力有限。
9. **数据分布风险**：当前主要是单板、单接口、单网络环境，跨设备泛化能力未验证。
10. **模型复杂度风险**：GNN 和多模态模型必须有匹配的数据结构，不能只因模型复杂而使用。

## 11. 推荐执行顺序

```text
1. 修复并验证持久化数据库路径
2. 通过 ARM64 容器构建统一版本
3. 仅部署 dist-arm64 产物到开发板
4. 重启验证数据仍保留并继续增长
5. 导出当前真实数据快照
6. 清洗数据并生成质量报告
7. 去掉 score，按时间切分数据
8. 训练规则基线、AutoEncoder、PyTorch Transformer
9. 用独立测试集比较模型
10. 用真实故障实验补充标签
11. 写正式 VIPLite 推理程序
12. 转换并部署 A733 .nb
13. 将结构化异常事件写入 SQLite
14. 接入本地知识库和历史案例 RAG
15. 最后再扩展 GNN、多模态和本地 LLM
```

## 12. 成功标准

### 工程标准

- 开发板重启后数据库不丢失
- systemd 只有一个服务实例
- 开发板不包含源码和构建中间文件
- NPU 推理程序能读取原始输出
- 服务端能记录模型版本、推理耗时和异常结果

### 模型标准

- 测试集按时间独立划分
- 不把 `score` 作为正式模型输入
- 明确区分规则标签和人工标签
- 报告误报率、漏报率和每小时告警数
- 与现有规则和传统 ML 基线比较

### 产品标准

```text
检测异常
  → 给出异常类型
  → 展示证据
  → 检索相似历史事件
  → 生成根因排序
  → 给出处理建议
  → 保留不确定性说明
```

达到这些标准后，项目才可以从“端侧模型流程验证”升级为具有实际研究和工程价值的“端侧智能网络诊断系统”。
