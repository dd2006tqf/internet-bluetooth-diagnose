# V4l2FrameSource 真机长稳验证（10 分钟）

- 日期：2026-11-11
- 性质：**工作流外任务**（无新增表面、无产品代码改动）
- 前置：`2026-11-10-v4l2-real-board-bringup-and-tunnel-incident.md` 第八轮
  （产品路径首次端到端跑通，9 秒数据）

## 一、构思

接线（`V4l2FrameSource` → `server.cpp` 评估循环）会让视频归因从恒 `NONE`
变成可能非 `NONE`，是行为变更。但接线前手上只有 9 秒数据，不足以支撑
「这个帧源可以长期驱动生产判决」的判断。所以先做长稳当门槛：

- 若长稳不过 → 先修稳定性，接线推后；
- 若长稳通过 → 接线才有依据。

长稳只看帧元数据（`seq_gaps` / `gap` / `bytes` / fd / RSS），
**不依赖画面亮度**——本轮环境较暗、画面 mean 仅约 11.8，但这不影响
帧统计结论的有效性。

## 二、实现方式

探针 `/tmp/prodprobe.cpp`（一次性，未入库）直接链接
`server/src/v4l2_frame_source.cpp` + `media_topology.cpp`，交叉编译上板。

- 配置：`1640x1232 @ 30fps, buffer_count=4, window_ms=1000`
- 时长：600 秒
- 用法：**连续调用** `snapshotAndReset()`（该函数自身阻塞一个 `window_ms`）。
  若在两次调用之间 `sleep`，队列积压会让 `max_gap` / `p95_dequeue`
  假性飙到 400~500ms、fps 掉到 11——这是探针用法错误，不是产品缺陷。
- 另起循环每 20 秒采样 `/proc/<pid>/fd` 与 `VmRSS`，观察泄漏。

## 三、结果

```
== 汇总 ==
cfg=1640x1232@30 buf=4
elapsed=600.83s windows=601
received=11152 expected=18030 (loss_expected=38.15%)
REAL_FPS=18.56  BANDWIDTH=53.9 MiB/s
corrupt=0 seq_gaps=0 max_gap=54ms p95_gap=54ms p95_dequeue=2ms
```

| 指标 | 9 秒（第八轮） | 10 分钟（本轮） | 结论 |
| --- | --- | --- | --- |
| REAL_FPS | 18.58 | **18.56** | 无漂移 |
| 带宽 | 54.0 MiB/s | **53.9 MiB/s** | 无漂移 |
| seq_gaps | 0 | **0**（601 窗口） | 零丢帧 |
| corrupt | 0 | **0** | 零坏帧 |
| max_gap | 54 ms | **54 ms** | 稳定 |
| p95_dequeue | 0~1 ms | **2 ms** | 用户态未拖慢 |
| fd | 3 | **3**（首尾一致） | 无 fd 泄漏 |
| RSS | 584 kB | **584 kB**（首尾一致） | 无内存增长 |

**长稳通过。** 接线的前置条件（稳定性）已满足。

## 四、同时确认的两件事

### 1. 全黑帧的根因是镜头保护膜，不是缺陷

撕膜前：`mean=0.7`、`max=60`、`nz>16=0.1%`、帧间差 0.5~0.9（噪声级）。
撕膜后：`mean=11.8`、`max=252`、`nz>16=11.0%`、帧间差 1.8~9.9。

`max` 从 60 到 252 是决定性差异。因此「全黑」不是 sensor/ISP 故障，
而是物理遮挡。**以后遇到全黑先查镜头遮挡，不要先怀疑驱动。**

（画面仍偏暗是环境照度低所致，用户已确认；不影响帧统计。）

### 2. 预览命令（可复用）

```bash
ssh board 'gst-launch-1.0 -q v4l2src device=/dev/video0 num-buffers=5 \
  en-awisp=1 en-largemode=0 \
  ! "video/x-raw,format=NV12,width=1640,height=1232,framerate=30/1" \
  ! videoconvert ! jpegenc quality=92 \
  ! multifilesink location=/tmp/snap_%02d.jpg max-files=5'
scp board:'/tmp/snap_*.jpg' ~/camera-preview/
```

约束：`en-awisp=1` 不可省（省掉零帧）；分辨率只能是 1640x1232 或
3280x2464（1280x720 / 1920x1080 / 640x480 均零帧）。

## 五、接线前的阻塞问题（已定位，未解决）

`video_qoe_attribution.cpp:46` 用 `frames.frameLossRatio() >= /* 0.05 */`
判定画面差。而本板 `frameLossRatio()` **恒为约 0.3815**——因为驱动
`G_PARM` 报 30fps、实际只出 18.56fps，`frames_expected` 按 30 推算。

**直接接线会让画面侧恒定判为「差」**，归因持续输出 `APP_LAYER_SUSPECT`
或类似结论，而实际链路 `seq_gaps=0` 完全健康。这是假告警，必须在接线
前解决。可选方向（需决策，均属行为变更）：

1. 用实测帧率回填 `cfg.fps`（但 `G_PARM` 回报仍为 30，现有回填逻辑不触发）；
2. 在帧源内按「驱动名义 vs 实际」做基线校准，输出**相对**丢帧率；
3. 接线处对已知基线偏差做补偿后再生效。

## 六、变更文件

| 文件 | 性质 |
| --- | --- |
| `openspec/tasks/2026-11-11-v4l2-frame-source-soak.md` | 本文件 |

产品代码零改动。探针与脚本均在 `/tmp`，未入库；预览图存于
`~/camera-preview/`（仓库外）。
