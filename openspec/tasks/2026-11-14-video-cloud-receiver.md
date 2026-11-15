# 视频留证：编码器接入生产循环 + 云端接收端

> 日期：2026-11-14
> 分类：工作流外实施
> 结果：**编码消费者已接入采集循环（代码级）、云端接收端已完成并通过
> 328 项全量测试**；板端发送端未实现，真机端到端未验证

## 构思

### 这一步要解决什么问题

上一条记录（`2026-11-14-video-encoder-cedar-ve.md`）留下两条遗留：

1. 编码器没接进生产采集循环——中枢只有 Qt 面板一个消费者；
2. 推流传输与网络场景判断没开始。

用户当轮的指示有三条关键信息：

> 开发板已经在线了，开始完整做吧；对于你列出的不做什么，持保留态度；
> 你说的那个实时查看，是指 qt 面板上可以实时查看吗？如果是的话就做，
> 如果是指云端实时查看，那可以先不做；**云端接收端也由我们这个对话框来做**。

于是范围确定为：**Qt 面板实时查看 = 做**（已有帧中枢租约路径，本次接编码器
不改变它）；**云端实时查看 = 不做**；**云端接收端 = 做**。

### 为什么编码消费者要独立线程

`IVideoEncoder` 的头文件里写死了「不保证线程安全，一个实例一个消费者线程」。
面板的 `video_fetch_thread` 已经在跑，编码器塞进去会让两个消费者争同一个
对象的生命周期。所以 `ServerContext` 多一个 `video_encode_thread`，编码器
实例是该线程的**局部变量**。

线程里也不直接暴露 `peekStats()` 给别的线程——那是未加锁的普通结构体，
外部读就是数据竞争。等归因层需要，再由该线程往带锁快照槽写。

### 为什么生产接入用 memcpy 而不是零拷贝

零拷贝在本板几何上确实成立（UV 偏移 2030336 == `align16(1640)×1232`）。
但硬件编码是**异步**的：`encode()` 返回时硬件可能仍在读那块 dma-buf，而帧
中枢的租约语义是「`release(slot)` 之后就可以覆写」。

零拷贝要成立，调用方必须持有租约直到**下一次** `encode()` 返回。这条义务
只有单测覆盖，从没在真机上验证过。默认值是 `allow_zero_copy=false`，
生产接入不背这条未验证的义务——省下的每帧 3 MB memcpy 换不来一个静默的
画面错乱。

### 为什么云端必须分片

片段是几十 MB（1640x1232 @ 2000 kbps，异常前后 90 s ≈ 22 MB），而所有
edge 上行共享 `MAX_TELEMETRY_BODY_BYTES = 1 MB`。抬高这个上限会同时抬掉
**每个** edge 端点的内存与解析代价——一次畸形请求就能让一个 worker 占住
几十 MB。

所以拆成两个端点：

1. `POST /network/edge/video-clips` —— 签名清单（几百字节）；
2. `POST /network/edge/video-clips/{clip_id}/chunks/{index}` —— 签名分片。

### 为什么分片用 base64 而不是裸二进制

裸 `application/octet-stream` 能让验签直接覆盖原始字节，看起来更省事。
但它会造出**第二套**鉴权约定：现有 `_read_verified_body` 靠报文里的
`device_id` 把签名绑定到设备；二进制报文里没有 JSON 可解析，设备身份就
只能改从 header 取，于是同一套 Ed25519 信任锚在两类端点上出现两种绑定
方式，审计时无法用一条规则解释全部上行。

代价是 33% 体积膨胀。留证是**事件触发**的偶发行为，不是持续推流，
这个代价换一套统一的鉴权语义是划算的。

## 实现方式

### 板端（C++）

**`server/include/server.hpp`** —— 加 `video_encode_thread` 与
`video_encode_stop`。后者与 `running` 分开：`running=false` 会停掉所有
监控，而这里只需要停这一个消费者。

**`server/src/server.cpp`** ——

- 采集块之后插入编码消费者线程：等 `published` 计数变化再 `acquire()`
  （不是每 20 ms 无脑轮询），`mmap` 租约 fd、`encode()`、`munmap`、
  `release(slot)`、`close(fd)`。
- 关停顺序写进代码：**先停帧源 → 再停编码线程 → 最后销毁中枢**。反过来
  的话中枢析构后两个线程仍持有裸指针，是 UAF。
- `~ServerContext` 加防御性 join：`std::thread` 析构时仍 joinable 会直接
  `std::terminate`，那会把「某一步初始化失败」变成「进程被 abort」，
  现场只剩 core 看不到真因。

一个刻意的自我纠正：第一版每 20 ms 无脑 `acquire()`。中枢发布上限是
10 fps，那会让 `describe()` 里的 `acq`/`rel` 膨胀到帧数的数倍——而这两个
计数正是判断租约协议有没有真被走通的**第一现场**。改成等 `published` 变化。

### 云端（Python）

**`network_assurance/video_contracts.py`**（新）—— `VideoClipManifest` /
`VideoClipChunk` / 两个 Result / 一个运营端 `VideoClipItem`。全部继承
`_ClosedModel`（`extra="forbid", frozen=True`），与既有 edge 契约一致。
`clip_id` 的取值域钉成 `validate_boundary_identifier` 的正则——它会直接
成为对象键的一段。

**`persistence/models.py`** —— 两张表：

| 表 | 主键 | 说明 |
|---|---|---|
| `network_video_clips` | `clip_id` | 清单事实；`tenant_id` 由 `TenantScopedMixin` 提供 |
| `network_video_clip_chunks` | `(tenant_id, clip_id, chunk_index)` | 已落盘分片，复合主键就是幂等保证 |

清单表**不存** `received_chunks` 和 `state`：两者都能由分片表推出，存成
列就多一个必须与之同步的计数器，少写一次同步就是一个永远卡在 PENDING 的
片段。代价是一次带索引的 count。

**`alembic/versions/0099_network_video_clips.py`** —— `down_revision =
"0098_network_edge_hypotheses"`，含标准的
`ENABLE`/`FORCE ROW LEVEL SECURITY` + `CREATE POLICY`。已用离线 SQL 渲染
验证过整份 DDL 正确。

**`network_assurance/service.py`** —— 四个方法：
`register_video_clip` / `store_video_clip_chunk` / `list_video_clips` /
`list_video_clip_chunks`。幂等三条规则：

- 清单重放 → duplicate；同 `clip_id` **异内容** → 冲突拒绝（证据不可变，
  不能让到达顺序决定内容）；
- 分片必须在清单之后（分片先到等于让服务端猜归属）；
- 同索引同内容 → duplicate；同索引异内容 → 拒绝（板端编号错乱）。

**`api/routes/network_assurance.py`** —— 四个端点。分片端点有三重校验，
**顺序不可换**：先解 base64（`validate=True`）、再比 `size_bytes`、
最后比 `sha256`。先比长度的话，一个只改了 padding 的报文能让两者都「看起来对」。
写入顺序是**对象存储 → 数据库**，不可颠倒：先写库再写对象会得到「行存在但
内容不存在」，那比孤儿对象糟得多——行会声称分片已送达，片段可能被判成
COMPLETE 而实际取不出内容。

**`object_store/keys.py`** —— 新增 `edge_video_chunk_object_key`，**没有**
复用 `quarantine_object_key`：后者的中间段语义是「事故草稿 id」、末段是
「媒体 id」，把 clip_id 当草稿 id 传进去会产生一批解析回来貌似合理但错误的
键，凡是枚举某草稿媒体对象的代码都会把这些当成成员。

### 为什么运营端读路由是必须的

只加 edge 写入端点、不加读端点，数据在平台里就没有任何出口——那才是真正的
「孤儿表面」：收得进来、看不到。所以同时加了：

- `GET /network/assets/{asset_id}/video-clips`（可按 `incident_id` 过滤）；
- `GET /network/assets/{asset_id}/video-clips/{clip_id}/content`（拼回码流）。

下载端点未收齐时返回 409 而不是半个文件：半段码流解不出画面，交给播放器
只会得到「文件损坏」这种无法归因的错误。

## 结果

### 已验证

- **x86 编译干净**，`ctest` **64/64 通过**（连跑两次，16.19 s / 16.45 s）。
- **ARM64 交叉编译干净**，产出 `build-arm64-native/server/weaknet-dbus-server`。
- 新增两组测试 **29/29 通过**：
  - `tests/test_video_clip_ingest.py`（19 项）：清单/分片幂等、冲突拒绝、
    越界拒绝、跨租户、收齐判定、运营端查询自洽；
  - `tests/test_video_clip_routes.py`（10 项）：认证、空列表、404 语义、
    非规范 asset_id、对象键往返、契约层 `extra="forbid"`、字段名钉死。
- **LMA 全量 328 项通过，2 项跳过**（79.22 s）。
- **迁移离线渲染验证**：两张表 + 复合主键 + 外键 + 两组 RLS 策略，DDL 正确。
- **迁移链单一 head**：`0099_network_video_clips`。

### 未验证 / 明确不做

1. **板端发送端未实现。** `encode()` 产出的码流目前只做计数，
   `produced` 分支里留了传输接入点的注释但没有实现。这是下一步。
2. **真机端到端未跑。** 开发板当前不在线（`ssh board` 连接被拒，
   `127.0.0.1:22222` 无监听）。编码消费者接入只做到「x86 与 ARM64 都编译
   干净 + 全量单测通过」，没有在真机上观察过 `[video] encoded=...` 那行日志。
3. **`flush()` 与零拷贝租约仍未真机验证**（与上一条记录同一遗留）。
4. **`tools/verify_telemetry_contract.py` 未注册视频契约。** C++ 侧发射器
   还不存在，现在注册只会得到一个永远失败的检查。等板端发送端落地后再加。
5. **整段码流的 `declared_content_sha256` 只做声明留存**，请求路径不回读
   全部对象校验。那是几十 MB 的同步 IO，会把一次偶发上行变成长事务。
   应由离线 worker 做——本模块不假装已经做过。

### 一个值得记的判断

`list_video_clips` 第一版对每个片段调了**两次** `_count_video_clip_chunks`：
一次填 `received_chunks`，一次算 `state`。除了让开销翻倍，它还在两次计数
之间留了一个「又有分片到达」的不一致窗口——`received_chunks` 说 2、
`state` 说 PENDING 这种自相矛盾的行会真的出现在接口上。改成只数一次。

### 为什么没走 OpenSpec change

用户明确指示 v4l2/视频线不走 change。按 `CLAUDE.md` 的「工作流外任务必须
留记录」，以本文件作为收尾记录。
