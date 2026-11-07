# 采集端热状态观测与归因（thermal_monitor）

> 日期：2026-11-06
> 分类：工作流外实施
> 结果：已完成（代码 + 本地验证 + 真机验证全部通过）

## 构思

### 要解决的漏洞

`observer_attribution`（2026-11-06 同批）已经把「eBPF 掉证据」分辨成两类：
CPU 被任务占满（`SELF_INFLICTED`）与 CPU 正常（`NOT_HOST_CPU_BOUND`）。
但它只看得见「忙」，看不见**第三种成因**：CPU 被硬件限频。

热节流时 `scaling_max_freq` 被 cpufreq cooling device 压低，同样的负载在更低
频率上只能完成更少工作，采集端照样掉证据——但运维该查的是散热/机柜环境，
不是业务进程，也不是链路。三者对现场的含义截然不同，归错一个就会在错误的
方向上排查一整天。

第二个业务作用：工业网关常年装在配电柜、机柜这类散热恶劣处，热状态本身
就是设备健康度的一部分。

### 为什么不走 OpenSpec change

用户 2026-11-06 明确要求「以后只要我不让你新开 change，你就默认不用新开」。
该偏好已写入项目 memory（`no-new-openspec-change-by-default`）。
本任务按工作流外实施推进，结果记录于此，**不具备 OpenSpec 验收效力**。

### 被否决的方案：新 MetricId + 第 21 个插件

`驱动面能力引入与业务融合方案.md` §4.3.3/§4.3.4 原提议：新增四个 `MetricId`、
一个第 21 个监控器插件线程、以及 `stats thermal` D-Bus 域。否决理由：

1. **孤儿接口**：`MetricId` 是封闭枚举，新增指标若没有真实读取方，就是纯粹的
   孤儿面。热数据唯一的消费者是「本轮观测可不可信」这一个判断，采一次用一次，
   没有任何其他读者。
2. **多一套配置面**：插件线程要带 `enabled` / `interval_ms`，要进
   `weaknet_config.cpp` 的五个位置、CLI 的 `list`、`serializeMonitorJson`
   的 valid 集合——为一个每 15 秒才被用到一次的值增加这么多表面积不划算。
3. **已有出口够用**：`weaknet-cli experience` 本来就是评估快照的观察面，
   §11.3「最小可行路径」也已认可这个复用。

### 分层设计（关键取舍）

`ThermalStateReader`（只做 sysfs I/O）与 `attributeThermalState`（纯函数）
**刻意不合并**。原因是：真实板子长期停在「28℃ / 未节流」这一稳定点，
节流分支与高温停转分支**在硬件上永远走不到**。如果判定逻辑和 I/O 混在一起，
这两条分支就只能是「看起来对」的死代码。切开后，节流、低温满载、高温停转
全部能用构造快照在单元测试里覆盖。

## 实现方式

### 新增文件

| 文件 | 内容 |
|---|---|
| `server/include/assurance/thermal_attribution.hpp` | 纯数据：`enum class ThermalAttribution` + `thermalAttributionToString()` |
| `server/include/thermal_monitor.hpp` | `ThermalZoneReading` / `ThermalSnapshot` / `ThermalThresholds` / `ThermalAttributionResult` / `attributeThermalState()` / `ThermalStateReader` |
| `server/src/thermal_monitor.cpp` | sysfs 读取实现 + 判定纯函数 |
| `server/test/unit/test_thermal_monitor_gtest.cpp` | 23 项：读取真值表 10 + 判定真值表 13 |

枚举单独抽一个头文件，是因为 `NetworkExperience` 是评估快照的纯数据结构，
只需要知道「归因结论是什么」，不应该为了一个枚举把 `<fstream>` / `<dirent.h>`
一路传染进快照序列化层。

### 修改文件

- `server/include/assurance/network_experience.hpp`：加 `thermal_attribution` /
  `thermal_attribution_detail` / `thermal_issues` 三个字段，与既有的
  `observer_attribution` 并列。
- `server/src/server.cpp`：在 `start_network_quality_thread` 的归因块（`decide()`
  之后）追加热状态采样与判定。**不触碰 `OverallPolicy`**。
- `server/include/assurance/legacy_adapter.hpp`：`toExperienceJsonV2` 序列化三个
  新字段——这是 `GetNetworkExperience` 的唯一出口，不序列化就是孤儿字段。
- `server/CMakeLists.txt`：`SERVER_SOURCES` 登记 `src/thermal_monitor.cpp`。
- `server/test/CMakeLists.txt`：两处登记（`add_executable` + `foreach` 列表）。
- `server/test/unit/test_w4_snapshot_lifecycle_gtest.cpp`：新增
  `ThermalAttributionEscapesThroughV2Json`，钉死字段确实能走出进程。

### 关键判定规则

| 维度 | sysfs 来源 | 为什么 |
|---|---|---|
| 温度 | `thermal_zone*/temp`，取**全部热区最高温** | 写死 zone0 会在换平台时静默读错分区 |
| 风扇 | `hwmon/hwmonN/{name,pwm1}`，按 `name == "pwmfan"` 匹配 | hwmon 编号由内核按注册顺序分配，重启/换内核会整体位移 |
| 占空比 | `pwm1 / 255` | hwmon ABI 满量程恒为 255，与 cooling device 的 `max_state`（本板为 4）是两套刻度 |
| 节流 | `sum(scaling_max_freq) / sum(cpuinfo_max_freq)` | **唯一**可靠信号 |

判 `scaling_max_freq` 而不判 `scaling_cur_freq`：后者低既可能是省电降频
（本板 ondemand 空闲时 policy6 = 416 MHz）也可能是被限频，两者同形；前者只由
cpufreq cooling device 改写。有一条测试专门钉死这一点
（`ThermalRead.LowScalingCurFreqIsNotThrottling`）。

阈值（`ThermalThresholds` 默认值，有实测依据）：

- `low_temp_full_speed_c = 45.0` / `full_speed_duty = 0.90`：本板 28~31℃ 就跑满速
- `high_temp_no_fan_c = 85.0` / `no_fan_duty = 0.10`：多数 ARM SoC 被动 trip 在 85~95℃
- `throttle_epsilon = 0.01`：吸收 sysfs 取整，避免边界每轮横跳

优先级 `THROTTLED > FAN_ANOMALY > NORMAL`：前者直接影响「这一轮观测可不可信」，
后者只影响设备寿命预期。但 `issues` 保留全部问题——enum 只能存一个值，
「风扇坏了但没节流」不该因此丢失。

异常才进 `warnings`（`THROTTLED` / `FAN_ANOMALY`）；`NORMAL` 与 `UNAVAILABLE`
不进，否则 warnings 每轮刷一条会被现场学会无视。`UNAVAILABLE` 单独走
`LOG_WARNING`。

### 验证手段

```bash
# x86 全量
cmake -B build-x86 -DCMAKE_BUILD_TYPE=Debug -DBUILD_EBPF=OFF && cmake --build build-x86 -j$(nproc)
ctest --test-dir build-x86/server --output-on-failure
# → 57/57 通过（原 56 套件 + 新增 1 套）

# ARM64 交叉
cmake -B build-arm64-native -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-arm64-cross.cmake \
      -DCMAKE_BUILD_TYPE=Debug -DBUILD_EBPF=ON && cmake --build build-arm64-native -j$(nproc)
# → 通过（现有 2 条无关 warning 一字未增）
```

### 文档同步

- `docs/网络体验评价体系.md`：新增「采集端热状态归因」小节（判定表、语义边界、
  本板实测策略异常、为什么分层）
- `docs/架构设计-模块与代码结构.md`：在「非插件常驻线程」段落后说明热采样
  为什么**不**做成插件
- `docs/ci-测试使用指南.md`：测试清单加一行

## 结果

### 已完成

- 代码落地并编译通过（x86 + ARM64 双架构）
- 23 项单元测试 + 1 项 V2 JSON 出口测试全绿
- x86 全量 57/57 通过，无回归
- 三份文档已同步

### 真机验证（2026-11-07 完成）

开发板重新上电后（`up 0 min`，确认之前的半休克状态确实是需要物理干预的系统级故障），
执行 `BOARD=board ./tools/ci.sh` 部署成功，rc=0，板端全部冒烟测试通过。

**板端 sysfs 实测值**（部署前采样，作为判定输入的独立佐证）：

```
thermal_zone7 skin_zone = 30000 (30.0℃)   ← 8 个热区中最高
hwmon8 name=pwmfan pwm1=255                ← 满量程 = 100% 占空比
policy0 scaling_max=1794000 hw_max=1794000 ← 未节流
policy6 scaling_max=2002000 hw_max=2002000 ← 未节流
```

**`weaknet-cli experience` 真机输出**（关键片段）：

```json
"thermal_attribution":"FAN_ANOMALY",
"thermal_attribution_detail":"Fan cooling policy is anomalous: hottest zone
  skin_zone at 30 C while fan duty is 100% (LOW_TEMP_FULL_SPEED);
  CPU frequency ceiling is normal at 100%, so this does not affect observation quality",
"thermal_issues":["LOW_TEMP_FULL_SPEED"]
```

与设计预期**逐字吻合**。三点值得记录：

1.  hottest_zone 正确选中 `skin_zone`（30℃），而不是写死的 `thermal_zone0`
    （`cpul_thermal_zone` 实测 24.1℃）——取最高温的逻辑在真机上生效了。
2.  同时输出「风扇异常」与「频率上限正常」，把设备寿命问题与观测质量问题
    在同一个结论里区分开，这正是 `THROTTLED > FAN_ANOMALY` 分层的用意。
3.  `warnings` 里也同步出现了该解释，且 `observer_attribution` 仍为 `NONE`——
    两个归因字段互不干扰。

**服务端日志**（`journalctl -u weaknet-server`）确认每 15 秒一轮稳定输出：

```
server.cpp:666] thermal attribution: FAN_ANOMALY temp=30.752C fan=yes duty=1 freq_ceiling=1 issues=1
server.cpp:666] thermal attribution: FAN_ANOMALY temp=39.432C fan=yes duty=1 freq_ceiling=1 issues=1
```

温度在 30~39℃ 之间真实波动（说明每次都是重新读 sysfs，没有缓存陈旧值），
`freq_ceiling=1` 与 sysfs 一致。`systemctl is-active weaknet-server` → `active`。

### 曾经的阻塞项：真机验证未能执行（已于 2026-11-07 解除）

开发板处于半休克状态，`ssh board` 在 banner 交换阶段超时。诊断结论：

- 云端 `127.0.0.1:2222` 监听 socket 存在（uid 1000，session-107.scope）
- 板端 4 条 SSH 载体连接（`120.85.135.53`）仍是 ESTAB
- 但发往板端的数据全部积压在 Send-Q（400/280/828 字节）无接收确认
- 大量 `CLOSE-WAIT` 说明云端已关闭的会话在板端未被回收

即：TCP 三次握手能完成，但板端用户态不再取走数据。这不是隧道没建起来，
是板端本身（sshd 或其所在系统）已经无法响应。

诊断期间状态继续恶化，最终确认为**隧道彻底消失**：

- 后台探测返回 `kex_exchange_identification: read: Connection reset by peer`
  （从超时变成对端重置）
- 随后 `127.0.0.1:2222` 已无监听（`Connection refused`）
- 板端载体连接由 4 条减到 3 条，Send-Q 固定在 204 字节不再变化
- 等待 55 秒（`weaknet-tunnel` 的重绑延迟）后重试 4 次，全部 `Connection refused`

`ssh` 已无任何入口，云端侧不具备进一步诊断或恢复的手段。

按铁律「遇阻塞必问，不得自作主张」，未对开发板或隧道配置执行任何写操作
（没有重启板子、没有改 unit、没有 kill 会话）。

### 遗留问题

1. ~~真机验证待做~~ **已完成（2026-11-07）**：`weaknet-cli experience` 真机读到
   `"thermal_attribution":"FAN_ANOMALY"` 与 `"thermal_issues":["LOW_TEMP_FULL_SPEED"]`，
   与预期逐字吻合，详见上文「真机验证」小节。
2. **`FAN_ANOMALY` 会成为本板的常态输出**：因为 IPA 的 `sustainable_power`
   只有 756/914 mW，远低于实际功耗，风扇策略本就过度冷却。这不是本模块的
   误报，是它在正确报告一个真实的策略问题。是否要为该板卡调 IPA 参数、
   或把阈值上调，需要单独决策（属于「写 sysfs」范畴，超出本模块只读纪律）。
3. **未做配置开关**：当前没有 `thermal.enabled` 之类开关，采样恒定执行。
   考虑到代价是每 15 秒三次 sysfs 目录读取（百微秒量级）且失败时只报
   `UNAVAILABLE`，暂不认为需要开关。若将来现场有异议再补。

### 是否升格为 OpenSpec change

建议**否**。这是一次增量观测能力的引入，不改变任何既有行为（默认态下
`NetworkExperience` 新字段为 `NONE`，与引入前逐字节等价）。若要升格，
需按铁律先 `git revert` 到变更前再走「freeze → 改代码 → evidence」流程，
成本远高于收益。
