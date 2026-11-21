# 云端事故静帧端点（`/edge/snapshot-images`）

> 日期：2026-11-20（云端契约）/ 2026-11-21（端侧接入 + 真机联调）
> 分类：工作流外实施
> 结果：已完成并**已在真机端到端验证**；对象存储为内存实现，持久化未验证

## 构思

### 要解决什么问题

板端已经有能力在事故触发时拍一张 JPEG 并落盘（见
`2026-11-20-snapshot-encoder-cedar-jpeg.md`），但那张图**出不了板子**。

用户选的方案 C 是「本地落盘 + 云端上传」。本地那一半已经通了，云端这一半
被契约挡死：`video_contracts.py:107` 把 `codec` 钉死为 `Literal["H264"]`，
而且全云端没有任何图片端点。

### 为什么不放宽 `codec` 枚举

契约作者的注释写得很明确：

> v1 只接受一种编码。放开之前必须先把整段校验与浏览器播放路径打通，
> 否则「收进来了但谁也放不了」比「拒绝」更糟。

这句话是对的，而且它拦的正是「改一行类型注解看起来很便宜」这个陷阱。
但更根本的问题是**语义不合**，不只是枚举不合：

| `VideoClipManifest` 字段 | 片段里的含义 | 一张静帧里是什么 |
| --- | --- | --- |
| `fps` | 码流帧率 | **无意义** |
| `expected_chunks` | 分片总数 | 恒 1，但仍要走两段式 |
| `started_at_ms` / `ended_at_ms` | 覆盖的时间窗口 | 时刻，不是窗口 |
| `declared_content_sha256` | 整段码流摘要 | 单张图摘要 |

后三行只是别扭，`fps` 那一行才是真问题：一个**恒被填成 25 的字段会被下游
当成事实读**，而它描述的东西不存在。所以另开契约，不复用片段那条链路。

### 为什么单张不分片

实测 IMX219 @1640x1232、quality=70 的 JPEG：

| 场景 | 体积 |
| --- | --- |
| 真实室内场景 | 118 ~ 128 KB |
| 满屏合成纹理（最坏情况） | 624 KB |

都在单分片上限内。分片是为几十 MB 的片段准备的机制，为一张 118 KB 的图
引入「清单 + 分片」两段式，只是把**两个失败点换成一个失败点**。

上限仍按 `MAX_VIDEO_CHUNK_BYTES` 对齐——它和片段分片共享同一个 1 MB 原始体
约束（base64 膨胀 33% 之后还要装得下），不是随手取的大数。

## 实现方式

### 改动文件

| 文件 | 内容 |
| --- | --- |
| `network_assurance/snapshot_contracts.py` | 新文件。`SnapshotImage` / `SnapshotImageResult` / 体积常量 |
| `persistence/models.py` | 新表 `NetworkSnapshotImageRecord`（一张图一行） |
| `object_store/keys.py` | `edge_snapshot_image_object_key()`，独立命名段 |
| `network_assurance/service.py` | `register_snapshot_image()` + `_snapshot_image_differs()` |
| `api/routes/network_assurance.py` | `POST /edge/snapshot-images` |
| `alembic/versions/0100_network_snapshot_images.py` | 新表迁移（含行级安全） |
| `tests/test_snapshot_image_routes.py` | 11 项传输层契约测试 |
| `tests/test_snapshot_image_ingest.py` | 10 项服务层语义测试 |
| `contracts/openapi.json` + 生成的客户端 | 契约产物 |

### 关键决策

#### 1. 一行一张图，不复用片段那对表

`network_video_clips` + `network_video_clip_chunks` 是一对多，因为片段必须
分片。静帧没有分片，也没有「收齐了没有」这个状态。塞进那对表会让
`expected_chunks` / `fps` 在图片行上变成无意义的占位值——而占位值会被
下游当事实读，这正是「两张表看起来都能用」这种诱惑的代价。

#### 2. 对象键用独立命名段

`tenant/{tid}/quarantine/snapshot-images/{image_id}`，不是
`.../video-clips/{image_id}`。理由与 `edge_video_chunk_object_key` 不复用
`quarantine_object_key` 完全相同：任何按前缀枚举的代码都会把另一个命名
空间的对象当成自己的成员。键里也没有分片号——静帧本来就是单张的。

#### 3. 写入顺序：对象存储 → 数据库

与分片端点一致且不可颠倒。先写库再写对象会得到「行存在但内容不存在」，
那比孤儿对象糟得多：行会声称证据已送达，而实际取不出内容。

#### 4. 三重校验必须在写对象存储之前全部通过

```
decode(base64, validate=True) → len == size_bytes → sha256 == content_sha256
```

顺序不能换。先比长度再解码，一个只改了 padding 的报文能让两者都「看起来
对」；先比摘要再比长度，截断后的载荷会报「内容被篡改」，而真实原因只是
板端截短了包——运维会去查一个不存在的攻击。

校验失败的载荷如果先落了对象，就会留下**孤儿对象**：没有行指向它，但它
确实占存储、也确实能被前缀枚举到。拒绝是可观测的，孤儿对象不可观测。

#### 5. 重放幂等，内容冲突拒绝

同一 `image_id` 重放 → `duplicate=True`，不新增行。
同一 `image_id` 但内容/事故/拍摄时刻不同 → 409。

证据不可变：覆盖之后「这个事故现场长什么样」取决于到达顺序。冲突判定只看
**板端自述、决定「这是哪张图」的字段**——`received_at` 这类服务端字段每次
都不同，拿它参与比对会让每次重放都变成冲突。

### 验证手段

```
cd Large-Model-Application
./.venv/bin/python -m pytest tests/ -q          # 351 passed, 2 skipped
./.venv/bin/python -m pytest tests/test_snapshot_image_routes.py \
    tests/test_snapshot_image_ingest.py \
    tests/test_video_clip_routes.py \
    tests/test_video_clip_ingest.py -q          # 52 passed
scripts/generate_openapi.sh --check              # 通过
scripts/generate_openapi_client.sh --check       # 通过
```

模型列与迁移列逐项比对一致（16 列），迁移链 `0100` 是唯一 head。

## 结果

### 已完成

- 独立契约 `network.edge.snapshot-images.v1`，`codec` 只接受 `JPEG`；
- 新表 + 迁移（含行级安全）+ 对象键 + 服务层 + 路由，全链路打通；
- 21 项新增测试；云端全量 **351 passed / 2 skipped**；
- OpenAPI 契约与客户端产物已同步，两个 `--check` 门通过。

### 端侧接入（同一任务的后半段）

#### 为什么复用 `VideoClipExporter` 而不另写一条上行通路

板端已经有一条完整的 edge 上行：Ed25519 签名、四个 `X-Edge-*` 头、
`edge.timeout_ms`、`RetryPolicy{8 次 / 1s→60s 指数退避}`。再写一条会有
**两套凭据加载、两套超时默认值、两套重试语义**，而它们不会报错——只会
慢慢漂移，直到某天出现「遥测传得上去、静帧传不上去」这种谁也说不出原因
的状态。所以静帧只是这条通路上的第二个入口。

#### 改动文件

| 文件 | 内容 |
| --- | --- |
| `include/edge_snapshot_upload.hpp` | 新文件。`EdgeSnapshotUpload` 自述 + `ISnapshotUploader` 单方法接口 + `SNAPSHOT_MAX_BYTES` |
| `include/video_clip_exporter.hpp` | 继承 `ISnapshotUploader`；`deriveSnapshotsUrl` / `buildSnapshotBody`；静帧队列与计数 |
| `src/video_clip_exporter.cpp` | 静帧入队/上传实现；`run()` 改为**静帧优先**；补上 `clips_enqueued` 从未自增的缺陷 |
| `include/snapshot_service.hpp` | `setUploader()` + `makeImageId()` + `uploaded_handoff` 计数 |
| `src/snapshot_service.cpp` | 落盘成功后把**内存里那份** `img.bytes` 交给上传器 |
| `src/server.cpp` | exporter `start()` 成功后接入静帧服务；停机日志补静帧段 |
| `test/unit/test_video_clip_exporter_gtest.cpp` | 13 项静帧契约测试（新增）+ 临时 Ed25519 密钥生成器 |
| `test/unit/test_snapshot_service_gtest.cpp` | 5 项上行交接测试（新增）+ `FakeUploader` |
| `test/CMakeLists.txt` | 给 exporter 测试补 `WEAKNET_HAVE_TLS`（否则两条正向用例静默 SKIP） |

#### 关键决策

**1. 抽 `ISnapshotUploader` 而不是让 `SnapshotService` 认识 exporter**

`SnapshotService` 的职责是「取一帧 → 编码 → 原子落盘」，与云端、凭据、
HTTP 全无关系。直接 include `video_clip_exporter.hpp` 会让
`test_snapshot_service_gtest` 因为本机没装 OpenSSL 而红——而它要验的是
「落盘是否原子」「租约是否归还」。窄接口让 10 项既有测试一个字都不用改。

`setUploader()` 而不是构造参数，同理：现有单测全都用三参数构造，把上传塞
进去会迫使每一处都构造假上传器，那种噪音会掩盖真正要断言的东西。

**2. 先落盘、后上传，且用内存里的字节**

落盘是本机事实，不依赖网络，永远不会因为云端不可用而丢。反过来的顺序会让
一次成功的上传提前把 `attempts.done` 置上，于是**写盘失败的那张图再也不会
重拍**。

上传用的是 `std::move(img.bytes)`，不是按 `path` 重读：`path` 所在目录将来
可能被清理策略扫掉，重读会在最需要证据的时候静默失败，而失败点离事故已经
过去很久。

**3. 上传失败不改 `handleOne()` 的返回值**

本地已经留住了证据。把云端不可达报成「拍证失败」会让运维去查摄像头，而
摄像头一切正常。上传侧成败由 exporter 自己的计数器表述，两套计数分开。

**4. 静帧按条数限流，不共用片段的字节配额**

片段可能几十 MB，静帧只有 120 KB。共用字节配额时，一个待传的 22 MB 片段
会把配额吃光，于是**事故现场那张唯一的图反而被丢**。静帧上限 4 条——正常
不会同时有第二张，能排到 4 张已经说明上行堵了一阵。

**5. `run()` 里静帧优先于片段**

一条 22 MB 片段在跨公网 RTT 234 ms 的链路上要传几十秒，而一张 120 KB 的
静帧通常一秒内就完。反过来会让「事故发生时恰好有一段片段在传」变成「现场图
排在几十秒后面」——而事故往往就是网络最差的时候。

**6. 报文与签名在入队时算好一次**

云端按内容判幂等：同 `image_id` 异内容 = 409，而客户端只会看到「上传失败」。
重算一次 JSON 若字段顺序或 base64 有漂移，同一张图会被判成内容冲突永久卡死。
所以 `PendingSnapshot` 里存的是**预序列化的 body + 对它的签名**。

**7. 一个顺手修掉的既有缺陷**

`VideoClipExporterStats::clips_enqueued` 声明了却**从未自增**，于是停机日志
里这一项恒为 0——「入队了没有」与「入队后又丢了没有」在日志上无法区分。
补上自增后，静帧与片段同一口径。

#### 验证

```
cmake --build build-x86 -j$(nproc)                        # 通过
ctest --test-dir build-x86/server --output-on-failure     # 70/70 全通过
cmake --build build-arm64-native -j$(nproc)               # ARM64 交叉编译通过
```

新增 18 项测试（13 项 exporter + 5 项 service）。两条使用真实 Ed25519 密钥的
正向用例**曾经静默 SKIP**——`WEAKNET_HAVE_TLS` 只 `PRIVATE` 定义在
`server_lib` 上，没有传播到测试编译单元，于是 `makeTempEd25519Key()` 走
`#else` 返回空串、用例走 `GTEST_SKIP`。补上 `test/CMakeLists.txt` 的宏之后
才真的跑起来。这类「看起来绿但根本没跑」的用例比没有用例更危险。

### 真机端到端验证（2026-11-21，已完成）

开发板恢复在线后按「部署 → 构造事故 → 观察 → 云端核对」走了一遍。

#### 前置：云端需要重启才有新路由

线上 `weaknet-cloud-api.service` 是 2026-11-14 启动的，`GET /edge/snapshot-images`
返回 **404**（路由不存在）。重启后变 **405**（路由在、但不接受 GET）。

数据库也要迁移：真实 Postgres 停在 `0099`，`alembic upgrade head` 到 `0100`。
这一步只建新表，不动既有表，风险低。

#### 板端一次事故的完整链路（实测日志）

```
03:01:45 快照已留存 incident=sitinc_...1795230092765
         bytes=119573  1640x1232  seq=7478  encode_ms=16
03:01:45 静帧已入队 image_id=snap-1795230105190-7478 body_bytes=159858
03:01:46 静帧已送达 image_id=snap-1795230105190-7478 jpeg_bytes=119573 body_bytes=159858
```

编码 16 ms、从入队到送达 1.4 秒。`body_bytes / jpeg_bytes = 1.337`，
与 base64 的 4/3 膨胀完全吻合——说明计数口径没混。

启动日志也确认接上了：

```
事故静帧留证已启用: ... upload=off          ← 服务创建时（exporter 还没起来）
静帧云端上行已接入: url=http://81.71.76.133:8000/api/v1/network/edge/snapshot-images
```

#### 云端落库（真实 Postgres）

```
image_id            | snap-1795230105190-7478
incident_id         | sitinc_radxa_cubie_a7a_radxa_cubie_a7a_1795230092765
codec / w x h       | JPEG / 1640x1232
size_bytes          | 119573
object_key          | tenant/tenant-m1-demo/quarantine/snapshot-images/snap-1795230105190-7478
content_sha256 前缀 | 5fc7b202a5742f49
```

把板端那张 JPEG 取回来算 sha256：`5fc7b202a5742f49c824b43b...`——**逐字一致**。
这是字节级证据，不是「字段看起来对」。

#### 四条语义各测一次（真实签名，非 mock）

用板端私钥在云端做真实 Ed25519 签名重放：

| 场景 | 期望 | 实测 |
| --- | --- | --- |
| 同 image_id 同字节重放 | duplicate，不新增行 | `200 {"accepted":false,"duplicate":true}`，行数仍为 1 |
| 同 image_id 不同字节 | 409 | `409 edge_snapshot_image_conflict` |
| 签名对不上 | 401 | `401 edge_authentication_failed` |
| 载荷被篡改（摘要不符） | 422 摘要错 | `422 edge_snapshot_image_digest_mismatch` |

最后再查一次库：仍然只有那 1 行，被拒的请求**没有留下任何行**。

### 一个必须说明的验证缺口

线上 rehearsal 服务装配的是 `InMemoryTenantObjectStore`
（`scripts/rehearsal_server.py:350`）：**对象只存在进程内存里，重启即丢**。

后果是：

- ✅ 行的字段与板端事实逐字一致；
- ✅ 内容摘要与板端字节一致（板端自己算的，被云端存下来）；
- ❌ **无法从云端把对象取回来验证**——没有持久化后端，也就没有「取回 → 重算
  sha256 → 比对」这一步。

所以「写入顺序：对象存储 → 数据库」这条设计**在真机上没有被真正检验**：
那个 503 分支（对象写失败就不写库）在内存实现下几乎不会触发。要补这条证据，
必须先把服务切到真 MinIO（或在 rehearsal 里加一个落盘实现）。

不得据此宣称「静帧证据已可长期取回」。已落库的这 1 行会在下次服务重启后
留下一个指向不存在对象的 `object_key`——这恰好是本文档第 3 条关键决策
（先写对象再写库）想避免的那种状态，只是成因不同（不是顺序错，是后端不持久）。

### 遗留问题

1. ~~端侧还没接~~ —— 已接（见上）。
2. ~~上游凭据路径未验证~~ —— 已验证：复用 `config_.edge.*`，
   `private_key_path` 为空时 `enqueueSnapshot()` 立即拒绝并计入 dropped
   （配置问题不该排进队列等重试）。
3. **运营端还没有查看入口**。图在对象存储里可按前缀枚举，但没有列表 API，
   也没有前端入口。要「能看见」还需要一个 `GET /assets/{asset_id}/snapshot-images`。
4. ~~未做端到端联调~~ —— 已做（见「真机端到端验证」）。真 Postgres 已验；
   对象存储仍是 `InMemoryTenantObjectStore`，**「取回对象再算 sha256」这条
   没做成**（见「一个必须说明的验证缺口」）。
5. **`hardware_disconnected` 判定未进生产 `V4l2FrameSource`**（与本任务无关，
   但静帧要「拍到」的前提是摄像头在出帧）。

### 一个必须说明的副作用

`contracts/openapi.json` 的重新生成**顺带修掉了一处与我无关的既有漂移**：
`video_contracts.py` 在 `ee0b823` 加入时没有同步更新该产物（HEAD 版本里
`video-clips` 出现 0 次），于是 `generate_openapi.sh --check` 一直是红的。
生成之后 diff 是 **1005 增 / 174 删**，其中绝大部分属于这块既有漂移，
不是本次新增端点造成的（新增的 `snapshot-images` 只有 1 个 path）。

不能只提交自己那部分——那个产物是单一整体，拆开提交会让 `--check` 处于
中间态。所以这次要么一起提交（顺带修了别人的 CI 红灯），要么整体不提交。

用户已决定：**一起提交（顺带修复）**。提交信息里如实写明其中夹带了这块
既有漂移的修复，好让日后 `git log` 能把「为什么这个提交里有一千行与静帧
无关的 diff」对上号。

### 过程中的一次阻塞（已解除，留作记录）

端侧代码写完的那一轮，开发板恰好不可达：

```
$ ssh board 'uname -m'
ssh: connect to host 127.0.0.1 port 22222: Connection refused
$ ss -ltnp | grep 22222
(无监听)
$ systemctl status weaknet-tunnel-guard
[隧道守卫] 端口 22222 未监听：隧道未建立，板端会自行重连
```

守卫日志显示**自 2026-11-11 起**就未监听——十天前就断了，与本轮改动无关。
mDNS `.local` 也解析不到（云端与板子本就不在同一网段，属已知拓扑限制）。

当时只完成了编译与单测验证，如实记录为「未在开发板上运行过」。
板子恢复在线后按上一节走完了真机端到端，**这条阻塞已不成立**。

留下的教训是：`weaknet-tunnel-guard` 只在日志里报「端口未监听」，
不会让任何门禁变红——一条断了十天的基础设施看起来和正常运行时
没有任何区别。要发现它，只能主动去连。

### 是否升格为正式 OpenSpec change

暂不。这是新增能力，不改既有接口契约（`VideoClipManifest` 一个字没动），
端侧也只是一条独立入口（`ISnapshotUploader`），没有改动任何既有上行语义。
若后续要把静帧纳入运营端可检索的证据列表（需要新列表 API + 前端），再评估。
