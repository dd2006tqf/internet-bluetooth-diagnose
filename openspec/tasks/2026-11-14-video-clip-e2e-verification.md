# 视频留证：板端 → 云端端到端验证 + 编码器时间戳缺陷修复

> 日期：2026-11-14
> 分类：工作流外实施
> 结果：**已完成** —— 板端 cedar 编码 → 录制器 → 分片上传 → 云端落库全链路
> 真机跑通；过程中发现并修复了一个**静默卡死**级别的生产缺陷（编码器不回填
> 帧时间戳），并补齐了能真正打到该缺陷的回归测试。

## 构思

### 这一步要解决什么问题

`2026-11-14-video-cloud-receiver.md` 的结果段落写着：

> 板端发送端未实现，真机端到端未验证

两边的单元测试都绿了——云端 328 项、板端 exporter/recorder 一堆断言。但
**没有任何一项证明这些字节能真的被云端接受**：签名算法一致、字段名一致、
分片对得上，全都只是「我认为一致」。

留证链路的价值全在「事故发生后那段录像真的能看到」上。如果这条链只在
单测里通，那它在生产里第一次被触发（也就是真出事那次）才发现传不上去，
等于从未拥有过这个能力。所以必须真机端到端跑通一次。

### 为什么不走 OpenSpec change

沿用用户此前对 v4l2/video 这条线的明确指示：不进 change。另外本次是
**验证 + 缺陷修复**，不是新增表面：编码器、录制器、上传器、云端路由
都已在，没有新的 requirement 要写。

### 摄像头零帧 → 用合成帧源

摄像头当前零帧（sunxi TDM 的独立问题，见
`openspec/pitfalls/vencoder-*` 与 IMX219 系列记录）。但本探针要验的是
**传输契约**，不是画质：

- 码流能不能切分片、拼回来 sha256 对得上；
- 清单/分片字段能不能被云端 pydantic 模型接受；
- Ed25519 签名能不能过验签；
- 重试复用同一份字节能不能被判 duplicate 而不是 409 冲突。

这四件事与像素来源完全无关。反过来，用真实画面去验只会增加活动部件
（摄像头、驱动、曝光），让失败原因更难定位。所以探针**合成 NV12**，
其余每一环都是生产代码。

### 备选方案与取舍

| 方案 | 为什么没选 |
| --- | --- |
| 只跑单测 + 手工 curl 打云端 | 手工 curl 验不到签名、分片边界、幂等这三件事的组合；单测验不到「云端 pydantic 真的接受这套字段」 |
| 用真实摄像头 | 当前零帧，且会把采集问题混进传输问题 |
| 把探针加进 CMake / ci.sh | 一次性验证程序不该进部署产物。沿用 `board_encoder_check.cpp` 的先例：手工交叉编译 + 手工上板 |

## 实现方式

### 新增文件

| 文件 | 作用 |
| --- | --- |
| `tools/board_video_clip_e2e.cpp` | 端到端探针（合成 NV12 → cedar → 录制器 → 上传） |
| `server/test/unit/test_video_encoder_gtest.cpp` | 新增 `VideoEncoderTimestamps` 两组用例 |

### 修改文件

| 文件 | 改动 |
| --- | --- |
| `server/src/video_encoder.cpp` | 抽出 `InFlightMetaQueue`，两个后端共用；cedar `drainOne()` 回填 `sequence`/`timestamp_ns` |
| `Large-Model-Application/.env` | 补 `REHEARSAL_EDGE_PUBLIC_KEY`（本地复现用；常驻服务另有其值） |
| `docs/ideas/视频流产品化-远程查看与事件留证.md` | 模块现状表按实际进展刷新；传输路径补 8000 已放行的事实 |

### 关键决策一：`--url` 在内存里覆盖，不改生产配置

板端 `/etc/weaknet/config.yaml` 的 `edge.url` 指向 `127.0.0.1:8000`——那是
**板子自己的回环**，不是云端。为让探针打到真云端，最省事的做法是改配置文件，
但那是生产配置，改它等于让探针结果与生产状态互相污染。

探针改为在 `loadWeakNetConfig` **之后**覆盖 `cfg.edge.url`：配置校验
（含「明文 HTTP 到公网」策略）照常先跑一遍，覆盖发生在其后。这是刻意的
——探针是一次性验证工具，信任边界由操作者决定；生产路径仍受完整校验。

### 关键决策二（本次最重要）：编码器必须回填帧身份

这是本次发现的**真缺陷**，且是「静默卡死」而非报错型：

板端首跑时编码器全绿（`in=60 out=60 keys=4 failed=0`），而录制器
`produced=0 aborted=0`——既不产出也不放弃，就那么卡着。

根因：cedar 后端 `drainOne()` 只填了 `data/size/keyframe`，
`EncodedFrame::timestamp_ns` 一直是默认值 0。录制器拿它当「最新帧时刻」，
于是 `newest_mono_ms_` 恒为 0 →「后沿未到」恒成立 → 永不产出。
**整个过程没有任何错误、没有任何告警、日志里连一条警告都不会有。**

修法要点：

1. 硬件编码是**异步**的——`encode()` 返回的那块码流对应的不是刚送进去的帧。
   所以身份要靠一个 FIFO 传递，不能在 `encode()` 里直接抄。
2. 入队点必须在**确认进了硬件之后**（`AddOneInputBuffer` 成功）。失败的帧
   永远不会产出码流，提前入队会让后面每一帧错位一格。
3. 出队必须在 `if (out)` **之外**：「腾空间」路径（`drainOne(api, nullptr, false)`）
   会把码流直接丢弃，但那份身份同样已被消耗。只在需要元数据时才出队会让队列
   永久错位一格，症状是「时间轴轻微漂移」——比彻底失效更难发现。
4. 抽成 `InFlightMetaQueue` 类，**两个后端共用**（见下条）。

### 关键决策三：回归测试最初是假绿的，必须让两个后端共用同一份实现

补了 `VideoEncoderTimestamps` 用例后，我做了 RED 检查：把 cedar 的入队行注释掉，
**测试依然全绿**。

原因：桩后端自己另写了一份回填（它是同步的，直接抄 frame 字段就是对的），
于是「cedar 忘了回填」这件事在单测里永远打不中。

修法是把回填逻辑抽成 `InFlightMetaQueue`，桩后端也过同一条代码路径。桩本来
不需要它，但只有这样，针对它的断言才是针对生产代码的断言。

改完再跑 RED 检查（故意改坏 `pop()` 使它只出队不回填）：2 项失败，
失败信息正是「时间戳恒为 0 会让录制器静默卡死」。测试这才真正生效。

### 关键决策四：探针窗口下限必须大于一个 GOP

首跑还遇到过 `窗口内无 IDR（回溯 500ms），放弃本次录制`。这不是产品缺陷，
是探针参数错：GOP 默认「1 秒一个 IDR」= 18 帧 ≈ 1000ms，`pre_roll_ms=500`
永远短于 GOP，于是「窗口内无 IDR」是**必然**而非偶发。

改为 `pre_roll_ms=2000 / post_roll_ms=1500`，并在代码里写下这个下限关系，
避免下次又踩。

### 验证手段

**x86 全量单测**：`65/65 passed`（含新增 2 项时间戳用例）。

**RED 检查**：注入故障后 `2 FAILED`，失败点与预期一致。

**ARM64 交叉编译**：探针手工编译（无 CMake target），链接行对齐
`weaknet-dbus-server`：

```bash
SR=/opt/sysroot-bullseye-arm64
COMMON="--target=aarch64-linux-gnu --sysroot=$SR --gcc-toolchain=$SR/usr \
  -Wno-unused-command-line-argument -stdlib=libstdc++"
clang++-18 $COMMON -std=c++17 -O2 -g -isystem $SR/usr/local/include \
  -Iserver/include -Iserver/src -Ithird_party/allwinner-venc/include \
  -isystem $SR/opt/weaknet-libcurl/usr/include/aarch64-linux-gnu \
  -isystem $SR/opt/weaknet-openssl-1.1/usr/include \
  -c tools/board_video_clip_e2e.cpp -o /tmp/board_video_clip_e2e.o
clang++-18 $COMMON \
  -L$SR/usr/local/lib -L$SR/usr/lib/aarch64-linux-gnu -L$SR/lib/aarch64-linux-gnu \
  -Wl,-rpath-link,$SR/usr/local/lib -Wl,-rpath-link,$SR/usr/lib/aarch64-linux-gnu \
  -Wl,-rpath-link,$SR/lib/aarch64-linux-gnu \
  /tmp/board_video_clip_e2e.o build-arm64-native/server/libserver_lib.a \
  $SR/usr/lib/aarch64-linux-gnu/libdbus-1.so -lglog -lbpf -lpthread \
  $SR/usr/lib/aarch64-linux-gnu/libsqlite3.so -ldl \
  $SR/opt/weaknet-openssl-1.1/usr/lib/aarch64-linux-gnu/libssl.so \
  $SR/opt/weaknet-openssl-1.1/usr/lib/aarch64-linux-gnu/libcrypto.so \
  $SR/opt/weaknet-libcurl/usr/lib/aarch64-linux-gnu/libcurl.so \
  -o /tmp/board_video_clip_e2e
```

注意：sysroot 里 `libcurl` / `libssl` / `libcrypto` **没有无版本号的 `.so`
链接名**，必须写完整路径；`jsoncpp` 探针用不到，不要加。

**板端真机**：

```bash
sudo /tmp/board_video_clip_e2e --config /etc/weaknet/config.yaml \
  --url http://81.71.76.133:8000/api/v1/network/edge/telemetry --frames 60
```

### 云端侧：migration 0099 从未应用

首次真机上传恒 500。我先误判为「缺验签信任锚」，把 `.env` 加了
`REHEARSAL_EDGE_PUBLIC_KEY`；随后查 `/proc/<pid>/environ` 发现常驻服务
**本来就有** `/etc/weaknet-cloud/edge_pub.pem`，内容与板端导出的公钥一致——
诊断错了，已向用户澄清。

真因（用常驻进程的完整环境复现）：
`relation "network_video_clips" does not exist`。Alembic 停在
`0098_network_edge_hypotheses`，`0099_network_video_clips` 从未执行。

按用户明确授权（A 方案）处置：先 `--sql` 预览建表语句 →
`docker exec weaknet-postgres pg_dump` 备份（宿主无 `pg_dump`）→
`alembic upgrade head` → 现为 `0099_network_video_clips (head)`。

那个多余的 `.env` 键保留但把注释改成符合事实的说法（常驻服务另有其值，
仓库内路径只服务本地子进程复现），因为本地子进程复现确实需要它。

## 结果

### 端到端实测（板端输出）

```
[enc] in=60 out=60 keys=4 dropped=0 failed=0 flushed=0
[enc] codec bytes=178897
[rec] 时间轴 mono=[1,3278]ms 跨度=3277ms（60 帧）
[rec] 产出片段 clip_id=radxa-cubie-a7a-1794665768350-420928 bytes=130954
[self] chunks=1 sha=89ed3ca4c4a98b33...
[self] 分片拼回一致 OK（130954 B）
[up] uploaded=1 failed=0 dropped=0 manifests=1 chunks=1 send_failures=0
[done] 云端确认收齐
```

`[rec] 时间轴 mono=[1,3278]ms 跨度=3277ms` 这行是修复生效的直接证据——
修复前它是 `[0,0]`。

### 云端入库核对

```
clip_id: radxa-cubie-a7a-1794665768350-420928
asset_id: radxa-cubie-a7a   incident_id: e2e-probe-26845
1640x1232  total_bytes=130954  expected_chunks=1
declared_content_sha256=89ed3ca4c4a98b33635280a2ea9118b2932be8c25aaf468682094ba74573ff7b
```

分片行 `size_bytes=130954`、`content_sha256` 与清单声明逐字一致，也与板端
打印的 sha 前缀一致。

### 幂等矩阵（前序已验，本次未回退）

| 操作 | 结果 |
| --- | --- |
| 重发同一清单 | 200 `duplicate:true` |
| 重发同一分片（同字节） | 200 `duplicate:true` |
| 同 index 不同字节 | 409 `edge_video_clip_chunk_conflict` |

### 运维读面

`GET /api/v1/network/assets/radxa-cubie-a7a/video-clips` → 200，
`state=COMPLETE`；`?incident_id=…` 过滤生效。

### 清理

- `/tmp/edge_priv.pem`（板端签名私钥在本机的副本）已 `shred` 删除；
- 板端 `/tmp/board_video_clip_e2e{,.o}` 已删除；
- `Large-Model-Application/.env.bak.*` 被 `.gitignore` 的 `.env.*` 覆盖，不入库。

### 遗留问题

> **订正（同日晚，据板端运行态复核）**：初稿的遗留第 2、4 条写错了，已改写如下。
>
> - 初稿说「生产循环尚未真正触发过留证：什么条件算事故、由谁调 `startClip`
>   仍待接线」——**错**。`server.cpp:1766-1950` 与 `2063-2120` 里录制器、
>   事故巡检线程（轮询 `site_incident_correlator` 的活跃事故 → 写
>   `video_pending_clip_incident` → 编码线程消费并 `startClip`）、exporter
>   启动都已完整接线。真实差距是**从未有事故触发过**，不是没有触发代码。
> - 初稿说「摄像头仍零帧」——**不准确**。11-11 第七轮已用 `bin_factor=1`
>   打通 1640x1232 采集；产品默认几何就是这一档。当前板端日志里没看到
>   `video frame hub configured`，说明这几分钟内确实没拿到帧，但那是
>   **运行态问题（待查）**，不是「摄像头不支持」的结构性结论。

> **再次订正（2026-11-15，据真实事故触发的首轮运行）**：上一条订正把
> 「已完整接线」这句话下得太满了，其中 exporter 那一半**实际是断的**。
>
> - 断点：编码线程在 `server.cpp` 前半段启动时就捕获了
>   `ctx.video_clip_exporter.get()`，而上行器要到后半段才创建。捕获那一刻
>   必然是 nullptr，于是每一段片段都落进「上行器未启用」分支被静默丢弃。
>   录制、事故、日志全都正常，只有上传**永远不发生**，且日志里看不出这是
>   接线问题——这正是「代码存在」被误读成「代码生效」的典型形态。
> - 修法：改用原子槽位 `video_clip_exporter_slot`，`start()` 成功后发布、
>   `stop()` 之前撤下。用槽位而不是裸指针直读，是因为 `unique_ptr` 的写
>   与编码线程的读会构成数据竞争。
> - 教训：`startClip` 路径可读、可单测、能打出日志，全都不能证明它下游的
>   消费者拿到了对象。判断接线是否成立必须看**运行时行为**（上传日志），
>   不能只看代码存在。
>
> 首次完整跑通（同一条命令，见 `2026-11-15-incident-forge-and-exporter-fix.md`）：
> `视频留证已入队` → `视频留证开始上传` → `视频留证上传完成`，云端
> `state=COMPLETE`，下载回来 sha256 与清单一致且 NAL 含 SPS/PPS/IDR。

1. **`edge.url` 指向板子自己的回环，而 8000 在板端没有任何监听。**
   板端 `/etc/weaknet/config.yaml` 是 `http://127.0.0.1:8000/...`，
   `sudo ss -ltn` 无 8000。实测板端到云端 `81.71.76.133:8000` 是通的
   （405 = 端点存在但不接受 GET），到 `127.0.0.1:8000` 是 `http=000`。
   所以**生产路径的留证上行在当前配置下必然失败**——本次 E2E 是靠
   `--url` 覆盖绕过去的，生产配置本身没修。**已修**：2026-11-15 起板端
   `edge.url` 直连 `http://81.71.76.133:8000/api/v1/network/edge/telemetry`。
2. **探针未纳入 CI**，是刻意的（一次性验证）。这也意味着本次链路没有
   持续回归保护——若云端契约再变，只有单测能挡一部分。
   **部分缓解**：`tools/weaknet-incident-forge` 已成为一条可重复的端到端
   回归入口，但它需要真机与公网云端，仍不适合放进 CI。
3. **云端分片存在内存对象存储**（rehearsal 服务用
   `InMemoryTenantObjectStore`），重启即丢分片；元数据在 PG 里存活。
   生产部署需换成持久对象存储。
4. **生产路径从未被真实事故触发过**（代码已接线，见上方订正）。
   触发后的完整链路（含上传重试、队列背压）仍只有探针级验证。
   **2026-11-15 更新**：已用 `weaknet-incident-forge` 触发真实事故并跑通
   全链路；但这是「构造事实 → 生产判定」的路径，物理世界的真实异常
   （真实设备掉线）仍未被观测到，上传重试与队列背压也未施加压力。

### 是否值得升格为 OpenSpec change

不建议。缺陷已修、测试已补、端到端已验，没有新的产品表面要规划。
遗留第 2 条（生产循环触发条件）如果要做，那才是需要规划的部分——它涉及
「什么算事故」的产品定义，不是实现细节。
