# 云端事故静帧端点（`/edge/snapshot-images`）

> 日期：2026-11-20（云端契约）/ 2026-11-21（端侧接入 + 真机联调 + 运营端取回 +
> 落盘存储真机闭环 + 保留策略与运维入口）
> 分类：工作流外实施
> 结果：已完成。**真机 + 真 Postgres + 落盘对象存储**的端到端链路已跑通
> （板端私钥签名 → 公网 → 落库落盘 → 取回 → 板端重算摘要比对），验证了
> 跨服务重启的对象持久化，并补上了**按时间回收的保留策略 + 命令行入口**
> （默认 dry-run，先删行后删对象）。配额上限仍未做——见遗留 7 的边界说明。

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

后续一轮（2026-11-21，补运营端取回 + 落盘存储）：

| 文件 | 内容 |
| --- | --- |
| `object_store/local_disk.py` | 新文件。`LocalDiskTenantObjectStore`：三重路径防护 + 原子写入 |
| `object_store/integrity.py` | 新文件。`verify_retrieved_digest` + `DigestMismatchError`（判据与存储介质解耦） |
| `network_assurance/snapshot_contracts.py` | 新增 `SnapshotImageItem`（运营端摘要，不含 `object_key`） |
| `network_assurance/service.py` | `list_snapshot_images()` + `get_snapshot_image()` |
| `api/routes/network_assurance.py` | `GET /assets/{asset_id}/snapshot-images` 与 `.../{image_id}/content` |
| `scripts/rehearsal_server.py` | `REHEARSAL_OBJECT_STORE_DIR` 开关，默认仍是 InMemory |
| `tests/test_local_disk_object_store.py` | 20 项（路径穿越 / 软链接 / 原子性 / 取回摘要） |
| `tests/test_snapshot_image_operator_routes.py` | 15 项（列表语义 + 下载四类失败） |

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

2026-11-21 补运营端取回端点 + 落盘对象存储后重跑：

```
./.venv/bin/python -m pytest tests/ -q -m "not live"   # 386 passed, 2 deselected
./.venv/bin/python -m pytest tests/test_local_disk_object_store.py \
    tests/test_snapshot_image_operator_routes.py -q    # 35 passed
./.venv/bin/python -m ruff check …                     # 通过
./.venv/bin/python -m ruff format --check …            # 通过
scripts/generate_openapi.sh --check                    # 通过
scripts/generate_openapi_client.sh --check             # 通过
```

`contracts/openapi.json` 这一轮只增不改（+370 行，两个新 path），
没有上一轮那种夹带的既有漂移。

## 结果

### 已完成

- 独立契约 `network.edge.snapshot-images.v1`，`codec` 只接受 `JPEG`；
- 新表 + 迁移（含行级安全）+ 对象键 + 服务层 + 路由，全链路打通；
- 21 项新增测试；云端全量 **351 passed / 2 skipped**；
- OpenAPI 契约与客户端产物已同步，两个 `--check` 门通过。
- **运营端取回端点已补**（列表 + 下载，下载时重算摘要）；
- **落盘对象存储已补**（`LocalDiskTenantObjectStore`，跨重启存活）；
- 再新增 35 项测试；云端全量 **386 passed / 2 deselected**。

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

### 验证缺口已补（2026-11-21 后续一轮）

上一轮记下的那条边界是：rehearsal 装的是 `InMemoryTenantObjectStore`
（`scripts/rehearsal_server.py:350`），**对象只存在进程内存里，重启即丢**，
于是「取回 → 重算 sha256 → 比对」做不了，「写入顺序：对象存储 → 数据库」
那个 503 分支也从没被走到过。

这一轮把它补上了，三件事：

| 新增 | 作用 |
| --- | --- |
| `object_store/local_disk.py` | `LocalDiskTenantObjectStore`：真落盘，跨重启存活 |
| `object_store/integrity.py` | `verify_retrieved_digest` + `DigestMismatchError`，判据与存储介质解耦 |
| `GET /assets/{asset_id}/snapshot-images` 与 `.../{image_id}/content` | 运营端列表 + 下载；下载时**重算摘要并与行里的 `content_sha256` 比对** |

`rehearsal_server.py` 的装配改为读 `REHEARSAL_OBJECT_STORE_DIR`：设了就切落盘，
不设仍是 InMemory。默认行为一个字没改，CI 与既有跑法不受影响。两种模式的
装配都实测过（`type: LocalDiskTenantObjectStore` / `type: InMemoryTenantObjectStore`）。

#### 落盘实现为什么值得单独写，而不是“加个 dict 到文件”

三重路径防护，每一层挡的东西不同：

1. `key.assert_owned_by(context)` —— 租户边界；
2. 逐段拒绝 `""` / `"."` / `".."` / 反斜杠 —— 挡「键本身写得坏」；
3. `resolve()` 之后 `is_relative_to(root)` —— 挡「根目录内的某个中间目录
   是软链接」。前两层完全挡不住这一条，而一旦有人在存储根里造一条软链接，
   前两层就全废了。

写入用 `mkstemp(dir=path.parent)` + `write` + `flush` + `fsync` + `os.replace`：
临时文件必须与目标**同一文件系统**，否则 `os.replace` 退化成复制+删除、不再原子；
少了 `fsync`，ext4 delayed allocation 会留下「名字在、内容空」的文件——那比
0 个文件危险，因为它看起来像真图。

测试用真目录、真文件、真 fsync，不 mock 文件系统，20 项。其中两项直接钉住
上一轮没验过的性质：`test_round_trip_digest_matches`（写进去→取回来→重算→比）
与 `test_no_partial_file_survives_a_failed_write`（`os.replace` 抛错后目标路径必须不存在）。

#### 下载路径做摘要校验，而不是交给后台任务

证据被读取的那一刻是最需要确认它没变形的时刻。后台巡检能让问题更早暴露，
但替代不了这一刻的判据。

三条错误码刻意分开，因为处置完全相反：

| 情形 | 状态码 | 含义 |
| --- | --- | --- |
| 对象不存在 | 500 `..._object_missing` | 行说已送达但内容丢了，要查存储 |
| 长度不符 | 500 `..._size_mismatch` | 截断，比摘要更便宜也更确定的判据 |
| 摘要不符 | 500 `..._digest_mismatch` | 内容被换过，要查是谁改的 |

**都不能报 404**：404 会让运营端以为「这张图从来没有过」，而实际上它存在过、
丢了。前者什么都不用做，后者要立刻查。

#### 「503 分支」这个叫法是错的（2026-11-21 第二轮纠正）

上一轮把「对象写失败 → 不写库」记成「503 分支从未被走到」。补上之后发现
**它本来就不是 503**：

| 情形 | 状态码 | 谁负责 |
| --- | --- | --- |
| store 没装配（`app.state.object_store is None`） | 503 `object_store_unavailable` | 部署/装配错误，板端可重试 |
| store 写入失败（`put` 抛异常） | 500 `internal_error` | 存储故障，重试不会立刻好 |

路由里写对象那一行是裸的 `await object_store.put(...)`，没有 try/except。
`put` 抛什么都会一路冒到全局处理器，变成 `500 internal_error`。真正的 503
只来自 `get_object_store` 取不到 store，那是「存储没装配」，与「存储写失败」
是两件事。

状态码是次要的。**真正要守的不变量是「库里不能有行」。** 一条指向不存在对象
的行会声称证据已送达，而实际取不出内容——这比直接失败糟得多，因为它不可观测。

新增 `tests/test_snapshot_image_write_failure.py`（3 项）钉住这条：

| 用例 | 断言 |
| --- | --- |
| `test_object_write_failure_leaves_no_row` | `put` 被调用过、响应 ≥500、表里 0 行 |
| `test_object_write_failure_leaves_no_object_on_disk` | 失败路径不留任何残留文件 |
| `test_successful_write_still_creates_exactly_one_row` | **对照组**：换健康存储必须恰好 1 行且能取回校验 |

对照组不是凑数：没有它，上面两条「0 行」可能因为任何原因通过（签名没过、
路由 401 了，库里当然没有行）。

失败桩的实现本身踩过一个坑，值得记：第一版在 `_write_atomically` **成功之后**
再抛错，测出来磁盘上留了一个**完整**对象，于是「无残留」失败。那不是产品缺陷，
是桩选错了失败点——那个场景是「对象已送达、回执失败」，残留是必然且无害的
（孤儿对象，可回收；不会出现「有行没内容」）。最终把桩的失败点定在
`os.replace`，那才是 `_write_atomically` 的 except 清理分支唯一会被走到的地方。

#### 仍然没有验证的部分（不要把上面的当成“缺口全关”）

- ~~真机 + 真 Postgres + 落盘实现~~ —— **已验**（见下节「真机 + 落盘存储」）。
- ~~静帧的保留策略不存在~~ —— **已补**（见下下节「保留策略与运维入口」）。
  仍缺的是**配额上限**（按总字节数硬性封顶），保留期只能约束「多久以后删」，
  约束不了「写得多快」。

#### 真机 + 落盘存储（2026-11-21 第三轮，已闭环）

前两轮的缺口是「真 Postgres 验过，但对象存在进程内存里」。这一轮把云端切到
落盘实现后重跑，链路每一段都是真的：

```
板端私钥 → RFC 8032 签名 → 板端 curl 经公网 → 云端真 Postgres + 真落盘
→ 运营端取回 → 板端重算 sha256 与原始字节比对
```

签名工具是**纯标准库实现**（板端 Python 3.9 没有 `cryptography`，OpenSSL 1.1.1
的 `pkeyutl` 不支持 EdDSA）。用它对 RFC 8032 第 7.1 节 TEST 1 / TEST 2 两组
官方向量、以及与 `cryptography` 库交叉比对四项（种子 / 公钥 / 签名 / SPKI PEM），
全部逐字节一致之后才拿它当证据工具。

**先确认信任锚**：从板端真实私钥推导出的公钥
`MCowBQYDK2VwAyEA3qGprOxMQlNmiKZmxOSwr6SeSHk5F8bXq0ysBGvUzC4=`
与云端 `/etc/weaknet-cloud/edge_pub.pem`、仓库内 `secrets/edge_pub.pem`
**逐字一致**。这一步不能跳：公钥对不上时，后面任何签名失败都分不清是代码错
还是密钥错。

实测结果：

| 场景 | 期望 | 实测 |
| --- | --- | --- |
| 板端真实静帧上传 | 200 accepted | `200 {"accepted":true,"duplicate":false,"size_bytes":119573}` |
| 取回后板端重算摘要 | 与原始一致 | `True`，且 `got == data` 字节级为 `True` |
| InMemory 时代旧行下载 | 500 `object_missing` | `500 network_snapshot_image_object_missing`，`retryable:true` |
| 改 1 字节（长度不变） | 500 `digest_mismatch` | `500 ..._digest_mismatch`，`retryable:false`，详情给出两个摘要前缀 |
| 截断到 100000 字节 | 500 `size_mismatch` | `500 ..._size_mismatch`，`expected 119573 / actual 100000` |
| **重启服务后**取回 | 仍可取回 | `200`，`119573` 字节，摘要仍 `5fc7b202a5742f49` |

最后一行的对照价值最大：同一次重启之后，落盘写入的图能取回，而 InMemory
时代留下的那行仍然 `object_missing`。**持久化生效了，「有行没内容」也不会
因为重启而自愈。**

对象在磁盘上的布局也核对过：
`/var/lib/weaknet-cloud/objects/tenant/tenant-m1-demo/quarantine/snapshot-images/<image_id>`，
119573 字节，与行里的 `size_bytes` 一致，**没有 `.tmp-*` 残留**。

> 云端落盘目录是 `/var/lib/weaknet-cloud/objects`，通过 systemd drop-in
> 设 `REHEARSAL_OBJECT_STORE_DIR` 启用。这是**验证用**配置，不是生产存储方案。
> 保留策略（下一节）是**手工/调度触发的命令**，不是后台常驻任务：drop-in
> 里没有配任何 timer，磁盘不会自己变干净。


#### 保留策略与运维入口（2026-11-21 第四轮，已闭环）

落盘让磁盘占用单调增长，而写满的失败方式很隐蔽：`put` 抛 `OSError` → 上传端
500 → 运营端看到「这次事故没有图」，**没有任何一处说「磁盘满了」**。

| 文件 | 内容 |
| --- | --- |
| `network_assurance/snapshot_retention.py` | `SnapshotRetentionService` + `SnapshotRetentionPlan` + `SnapshotRetentionRefused` |
| `network_assurance/snapshot_retention_cli.py` | `industrial-ops-snapshot-retention` 入口 |
| `pyproject.toml` | `[project.scripts]` 注册 |
| `tests/test_snapshot_retention.py` | 18 项（误删防护 / 删除语义 / 出错残留） |
| `tests/test_snapshot_retention_cli.py` | 11 项（退出码 / 拒绝运行 / 报告字段） |

四个决策值得单独记：

**1. 删除顺序与写入顺序相反。** 写入是「对象 → 行」，删除必须是「**行 → 对象**」。
对称的理由：先删对象再删行，崩在中间就留下「行在、对象没了」的**谎报行**
（声称证据已送达，取回 500），而且不可观测；先删行再删对象，崩在中间留下
**孤儿对象**（浪费磁盘、可回收、不会说谎）。一句话：
**行是对「证据存在」的声明，绝不让声明比证据活得久。**

这条在实现时发现**测不出来**——详见
`openspec/pitfalls/delete-order-not-observable-from-final-state.md`：把顺序改反
之后 29 条用例全绿，因为 `_delete_objects` 吞异常，两种顺序收敛到同一最终态。
真正能区分的是**中途失败**，为此补了
`test_row_delete_failure_leaves_object_untouched`（让删行抛错，断言对象仍在），
并用「再改反一次看它变不变红」验证过它确实有效。

**2. 默认 dry-run。** 回收是不可逆的证据销毁，最容易发生的误用不是「忘了删」，
是「以为自己在看报告，其实在删数据」。真删必须显式 `--apply`。同
`weaknet-ota-ctl reconcile --dry-run`。

**3. 判据用 `received_at` 而不是 `captured_at_ms`。** 后者是板端自述墙钟，
一块没对过 NTP 的板子可以报出 1970 或 2099 年。拿它当判据等于「板子时钟错了
→ 证据被误删」。`received_at` 由服务端写入，板端改不了。

**4. 退出码不能用 2 表示「还有剩余」。** `argparse` 的用法错误本来就占用 2，
且它由 `parse_args` 直接抛 `SystemExit`，走不到 `main()` 的返回值。若两者共用
2，一个 `while rc == 2: 再跑一轮` 的调度循环会在**参数写错时永远转下去**——
而参数写错恰恰是第一次部署最常见的问题。最终：

| 退出码 | 含义 |
| --- | --- |
| 0 | 正常（含 dry-run 与「没有过期数据」） |
| 1 | 命令自己判定的参数拒绝（保留期 ≤ 0、batch 越界） |
| 2 | `argparse` 用法错误（缺必填项、不是整数），不被改写 |
| 3 | **真删除后**仍有剩余，可以再跑一轮 |

dry-run 恒返回 0，即使 `remaining > 0`：它一张都不删，跑一万轮剩下的一样多。
`remaining` 字段在两种模式下都照样输出，那才是表达「还有多少」的地方。

**不做的事**（写进模块 docstring，避免日后被当遗漏）：

- 不删事故。事故是时间线骨架，静帧只是附件。
- 不按「事故是否已关闭」筛选。那个状态可能永远不推进（没人手动关闭），
  于是磁盘照样写满。
- 不在库里记删除流水。这一版只写结构化日志，不新增表——新增表意味着一次
  迁移与一个新消费者，与「最小可验证闭环」不符。代价是日志不构成可对账的
  账本；真要做证据销毁审计时再升级成独立审计表。

验证：

```
./.venv/bin/python -m pytest tests/test_snapshot_retention.py \
    tests/test_snapshot_retention_cli.py -q      # 29 passed
./.venv/bin/python -m pytest tests/ -q -m "not live"   # 418 passed, 2 deselected
./.venv/bin/python -m ruff check …              # 通过
./.venv/bin/python -m ruff format --check …     # 通过（本轮新文件）
```

入口可执行性单独验过：`tomllib` 读出 `[project.scripts]` 那条，
`importlib.import_module` + `getattr` 能解析到 `<function run>`——不是
「写了就一定装得上」。

### 遗留问题

1. ~~端侧还没接~~ —— 已接（见上）。
2. ~~上游凭据路径未验证~~ —— 已验证：复用 `config_.edge.*`，
   `private_key_path` 为空时 `enqueueSnapshot()` 立即拒绝并计入 dropped
   （配置问题不该排进队列等重试）。
3. ~~运营端还没有查看入口~~ —— 已补：`GET /assets/{asset_id}/snapshot-images`
   （列表，支持 `incident_id` 过滤）与 `.../{image_id}/content`（下载 JPEG 并
   校验摘要）。前端入口仍未做（见 6）。
4. ~~未做端到端联调~~ —— 已做（见「真机端到端验证」与「真机 + 落盘存储」）。
   真 Postgres 已验；落盘实现的持久化、跨重启存活、三条错误码分支
   （`object_missing` / `size_mismatch` / `digest_mismatch`）都已在真机验过。
   对象写失败 → 不写库的不变量由 `tests/test_snapshot_image_write_failure.py`
   钉住（3 项，含对照组）；注意那是 500 不是 503，原因见「「503 分支」这个叫法是错的」。
5. **`hardware_disconnected` 判定未进生产 `V4l2FrameSource`**（与本任务无关，
   但静帧要「拍到」的前提是摄像头在出帧）。用户 2026-11-21 确认摄像头问题
   已解决（排线未插紧），镜头链路正常。
6. **前端还没有静帧查看入口**。API 已备，Web 侧未接——这是「能看见」的
   最后一公里，但不属于云端契约范围。
7. ~~静帧保留/配额策略不存在~~ —— **保留策略已补**（见「保留策略与运维入口」）：
   按 `received_at` 回收、默认 dry-run、先删行后删对象、退出码区分「拒绝 /
   用法错误 / 还有剩余」。
   **仍然没有配额上限**：保留期只能约束「多久以后删」，约束不了「写得多快」。
   一次采集风暴（比如阈值配错、板端反复重试）可以在一个保留期内把磁盘写满，
   而保留策略只在过期之后才动手。真要做配额，判据应当是**租户总字节数**
   而不是行数，且需要先决定「超限时的新写入是拒绝还是淘汰最旧」——那是个
   产品决策，不是顺手能加的开关。

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
运营端列表 + 下载端点这一轮也补上了，仍属于同一条链路的两端，
没有改动任何既有行为。
