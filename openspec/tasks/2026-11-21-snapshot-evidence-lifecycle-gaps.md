# 事故静帧留证链路的四处缺口收口（板端回收 / 保留期调度 / 配额 / 孤儿清扫 / 前端查看器）

> 日期：2026-11-21
> 分类：工作流外实施
> 结果：已完成（其中「真机端到端回收」受硬件缺失限制，边界见末节）

## 构思

### 起点：六处缺口

前序会话已经从「板端拍照 → 上传对象存储 → 插行 → 云端可查可取回」把主链路
打通。但主链路能跑通不等于它不会坏。逐项排查后列出六处缺口：

1. **板端磁盘没有回收**，云端有。板端 `SnapshotService` 把 JPEG 写在
   `/home/radxa/weaknet/data/snapshots/` 下，每来一次区域事故多一张约 120 KB
   的图，**永不删除**。
2. 保留期策略从未在真实 Postgres 上跑过（29 项测试全是 SQLite）。
3. 保留期没有调度器，`industrial-ops-snapshot-retention` 只是个命令。
4. 写入侧没有配额上限。保留期管的是「多久之后删」，不是「写得多快」。
5. 没有孤儿对象清扫器。模块 docstring 当时明确写着「不要假装它已经存在」。
6. 前端没有静帧查看器。`schema.d.ts` 里三个端点齐全，页面上没有任何静帧代码。

第 2、3 项在前序会话完成（真实 Postgres 上对隔离探针租户做了 dry-run 与
apply；systemd service + timer + 包装脚本，15 项包装测试全绿）。第 4 项
（配额）也在前序完成。本次收口剩下的第 1、5、6 项。

### 为什么不走 OpenSpec change

用户明确要求「把这些代码工作都做了」，没有要求走受管工作流；仓库规则
（`no-new-openspec-change-by-default`）也是除非明确要求否则走工作流外实施。
此外这几项改动横跨 C++ 板端、Python 云端、TypeScript 前端三套工具链，
放进一个 change 的 footprint 里只会让证据义务变模糊。

### 两个回收器的判据为什么必须不同（这一条决定了整个设计）

| | 板端 | 云端 |
|---|---|---|
| 判据 | `max_total_bytes`（总字节） | `retention_days`（年龄） |
| 为什么 | **板子没有 RTC**，墙钟可能大幅偏移 | 服务器时钟可信 |
| 默认 | `0`（关闭），unit 文件里覆写 2 GiB | 无默认，必须显式配 |
| 位置 | 每次成功写入之后立即执行 | 定时器（`OnBootSec`/`OnUnitActiveSec`） |

板端若按墙钟年龄判断，一块时钟跳变（没有 RTC，开机后 NTP 同步前是 1970 或
上一次的残值）会把**所有**图判成「太老」一次删光。所以板端只看目录总字节；
删除顺序用文件名里的毫秒——相对顺序不受全局偏移影响，全局偏移多少都不影响
「谁更旧」这个判断。

### 孤儿清扫器最危险的地方：上传竞态

孤儿定义是「库里没有行」，而「库里没有行」有两种完全不同的来源：

| 来源 | 该不该删 |
|---|---|
| 一次上传崩在 `put` 与 `insert` 之间 | **删** |
| 一次上传**正在** `put` 与 `insert` 之间 | **绝不能删** |

上传顺序是 `put(对象) → insert(行)`。清扫器若在这个窗口内枚举，那张正在上传
的图**就是**一个合法的孤儿。删掉它之后，上传方会插入一行指向不存在的对象——
正是整条链路一直在防的**谎报行**：声称证据已送达、取回 500、而且没有任何一处
会报错。

所以判据不能是「没有行」，必须是「没有行**且**对象足够老」。默认窗口 3600 秒，
比真实上传耗时（毫秒到秒）大三个数量级。

### 备选方案与取舍

- **板端用 config.yaml + `weaknet-cli set` 实时改上限？** 否。这是部署事实
  （这块盘有多大），不是运行期调参。实时可改意味着一次误操作就能把上限设成
  1 字节并在下一次事故时删光。环境变量 + unit 文件让改动必须走一次可审计的
  配置变更。
- **板端按墙钟年龄判？** 否，见上。
- **孤儿清扫器不设时间窗，靠「先删行再删对象」的顺序保证？** 否。那个顺序
  保证的是**删除**不产生谎报行；上传方向是 `put → insert`，是另一个方向的
  竞态，顺序保证不了。
- **孤儿清扫器顺手把「有行但对象不存在」的反向孤儿也删掉对齐？** 否。那是
  数据完整性事故，该报警、该人工介入。删了行就把「证据丢了」伪装成「从来
  没有过这张证据」——比留着那行更糟。

## 实现方式

### 板端容量回收（C++）

- `server/src/snapshot_retention.cpp` + `server/include/snapshot_retention.hpp`
  （前序会话已写好实现，本次补测试与注册）。
- `server/test/unit/test_snapshot_retention_gtest.cpp`：22 项测试 / 2 个套件。
  断言几乎全是「什么时候**不该**删」——默认关闭、等于上限不算超、绝不删刚写
  的那张、时钟跳变不会全删。本次新增
  `SnapshotScan.ParsesTheRealWallClockMillisecondForm`，钉住板端实测的
  **13 位**墙钟毫秒文件名形态（其余用例都用 1000/2000 这种 4 位数，位数不同
  会让「按位数判断」的实现露馅，而露馅方式是「所有 ms 解析成 0 → 按字典序删
  → 删错图」）。
- `server/test/unit/test_snapshot_service_gtest.cpp`：新增 4 项接线测试
  （20 → 24）。**接线测试的价值需要被证明**：模块自己的 22 项测试全绿，完全
  不能说明 service 真的调用了它。所以做了变异测试——把 `snapshot_service.cpp:513`
  的调用点换成一行注释后重编，`OverCapDeletesTheOlderImage` 与
  `NeverDeletesTheImageJustWrote` 立刻失败（2 failed / 2 passed）。恢复后重跑
  4/4 通过。
- `server/include/common.hpp`：补 `<cstdint>`、`<cstdlib>`（此前靠传递包含
  才能编译）。
- `tools/weaknet-server.service`：加
  `Environment=WEAKNET_SNAPSHOT_MAX_BYTES=2147483648`（2 GiB）。**没有这一行，
  功能就只是源码里存在，生产里是关着的。** 2 GiB 的依据：单张实测
  118~128 KB，约合 1.6 万张；`/home` 共 117 GB，占 1.7%。

### 云端孤儿清扫器（Python）

- 新增 `network_assurance/snapshot_orphan_sweeper.py`（331 行）。模块 docstring
  用图画出上传竞态，并显式列出「不做的事」。
- 协议从 `SupportsList`（只能枚举，删靠 `getattr` 兜底）收紧为
  `SupportsEnumerate`（`list_prefix` + `delete`）。理由写进了代码：一个「协议里
  没有却偷偷调用」的方法，会在换了实现的那天才炸，而那时炸的是**删证据**的路径。
- 三种存储实现都加了 `list_prefix`：
  - `local_disk.py`：前缀逐段校验 + `resolve()` 后确认仍在根目录内 + 只扫一层。
  - `memory.py`：整体重写，加平行的 `_first_seen: dict[str, datetime]`。静帧键
    是幂等键，重传会覆盖字节但**不是新对象**；年龄按首次写入算，否则一张被反复
    重传的孤儿会永远显得「刚写完」。
  - `minio.py`：`recursive=True`（默认会在 `/` 折叠，返回的是伪前缀而非对象）；
    `last_modified` 缺失时**跳过并告警**，不猜（猜 `datetime.min` 会让刚上传的
    对象立刻满足时间窗）。
- 新增 CLI `snapshot_orphan_sweeper_cli.py`，退出码与保留期 CLI 完全同形
  （0/1/2/3），并在 `pyproject.toml` 注册为 `industrial-ops-snapshot-sweep`。
- 新增 `tools/snapshot_orphan_sweep.sh` + `weaknet-snapshot-sweep.{service,timer}`。
  包装脚本独有的一道闸：**硬拒绝 `min_age_seconds < 60`**。清扫器自己只拒绝
  `< 1`，而 1 秒的窗口在安全上是荒谬的。

### 前端查看器（TypeScript）

- `web/lib/api/network.ts`：加 `SnapshotImageItem` 类型、
  `listAssetSnapshotImages()`、`getSnapshotImageObjectUrl()`。后者不复用
  `request()`——那个 helper 固定 `response.json()`，拿来读二进制会得到
  「Unexpected token 'ÿ'」，看起来像前端 bug、实际是拿 JSON 解析器读图片。
- 新增 `web/components/network/SnapshotPanel.tsx`（260 行）。三处要点：
  - **逐张按需取回**，不是一次性全拉（1640×1232 的 JPEG 解码后约 8 MB 位图）。
  - **必须自己 `revokeObjectURL`**：blob URL 不会被 GC 回收，翻十次页就泄漏
    几百 MB，而现象只是「用久了浏览器变卡」。
  - **取回失败要显式说出来**：`GET .../content` 的 500 语义是「已登记的证据
    取不回来」，这是整条链路唯一会承认「证据丢了」的地方。渲染成破图标就等于
    让这个信号消失。
- 接入两处：设备详情页新增「事故静帧留证」Tab；事故详情页插在「③ 证据」之后
  （同层：这是事故当时的画面本身，不是对事故的解释），按 `incident_id` 过滤。

### 验证手段

```
# C++ x86
cmake -B build-x86 -DCMAKE_BUILD_TYPE=Debug -DBUILD_EBPF=OFF && cmake --build build-x86 -j$(nproc)
ctest --test-dir build-x86/server --output-on-failure     → 71/71 passed
# 接线变异测试：删掉调用点 → 2 项失败；恢复 → 4/4 通过

# ARM64 交叉
cmake -B build-arm64-native -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-arm64-cross.cmake \
      -DCMAKE_BUILD_TYPE=Debug -DBUILD_EBPF=ON && cmake --build build-arm64-native -j$(nproc)
file build-arm64-native/server/test/test_snapshot_retention_gtest
      → ELF 64-bit LSB pie executable, ARM aarch64

# 部署（OTA 事务 PREPARED → SWITCH_PENDING → TRIAL → COMMITTED，releases/1.0.0 → 1.1.0）
BOARD=board ./tools/ci.sh --skip-test
ssh board 'journalctl -u weaknet-server -n 50 --no-pager | grep 静帧'
      → 事故静帧容量上限: 2147483648 bytes（从旧到新回收）
      → 事故静帧留证已启用: ... quota=2147483648 pruned=0 pruned_bytes=0
ssh board '<两个 gtest 二进制> --gtest_brief=1'      → 46 项全绿（retention 22 + service 24）

# Python
ruff format / ruff check                                → clean
pytest tests/test_snapshot_orphan_sweeper.py            → 24 passed
pytest tests/test_snapshot_orphan_sweeper_cli.py        → 12 passed
pytest tests/test_snapshot_orphan_sweep_wrapper.py      → 19 passed
pytest tests/ -k "snapshot or object_store or retention"→ 192 passed
pytest tests/ -m "not live"                             → 509 passed, 2 deselected
# 清扫器变异测试（5 条，全部被捕获）：
#   去掉时间窗判据        → 2 项失败
#   前缀去掉结尾斜杠      → 17 项失败
#   行查询去掉租户过滤    → 1 项失败（见下）
#   remaining 忽略删失败  → 1 项失败
#   dry-run 也真删        → 1 项失败

# 前端
pnpm typecheck                                          → 通过
pnpm build                                              → 通过
scripts/generate_openapi.sh --check                     → 无漂移
scripts/generate_openapi_client.sh --check              → 无漂移
```

> **板端这 46 项是怎么跑出来的（过程中抓到一个真问题）。** 第一次跑只有
> **45 项**（retention 21 + service 24）。原因不是新用例没写对，而是
> **`build-arm64-native` 里的二进制比源码旧**：源码 `19:22:50` 改的，
> 二进制 `19:15:28` 构建的，`strings` 里查不到新用例名。只重编单个 target
> 就会补上。教训是「构建时间戳早于源码时间戳」这件事没有任何提示——CMake
> 增量构建对着的是一个已经列在依赖里的文件，它不会告诉你产物其实是旧的。
>
> 修完后在板端实测：`22 tests from 2 test suites` + `24 tests from 7 test suites`，
> 全绿。板端跑用的是临时 scp 的二进制（生产 release **不含**测试二进制，这是
> 对的），跑完已删除；需要复现时重新 scp 即可。

### 一个被变异测试抓出来的真缺口

「行查询去掉 `tenant_id` 过滤」这条变异**第一轮没被捕获**（23 项全绿）。原因：
原用例是「另一个租户里有同名 image_id 的行」，但 `_seed_row` 让 `object_key`
跟着租户走，两个租户的键本来就不同，`IN` 子句压根匹配不上。那条用例测的是
「判据按**键**匹配而不是按裸 image_id」，本身有效，但**测不到租户过滤**。

补了一条新用例：构造一行「租户列是别人的、对象键却是本租户的」的**脏行**。只按
`object_key` IN 匹配的话，这行会把本租户一个真孤儿伪装成「有行」，于是永远清不掉
——静默的磁盘泄漏。补完后重跑该变异 → 如期失败。原用例的 docstring 也改成如实
描述它真正在守什么。

## 结果

### 已完成

- 板端容量回收：实现、22 项单元测试、4 项变异验证过的接线测试、ARM64 构建、
  部署上线并确认 `quota=2147483648` 生效；两个测试二进制（22 + 24 = 46 项）
  已在开发板上跑通。
- 云端孤儿清扫器：服务模块 + 三种存储实现的 `list_prefix` + CLI + 包装脚本 +
  systemd service/timer，24 + 12 + 19 = 55 项测试全绿，5 条变异全部被捕获。
- 前端静帧查看器：API 客户端 + 面板组件 + 两处接入，typecheck 与生产构建通过。
- `CLAUDE.md` 补「事故静帧留证」运维表（含「两个判据不一样」与「时间窗不能调小」
  两条警示）。

### 诚实的边界（不得淡化）

1. **板端「真实事故触发真实回收」没有被验证。** 现场**摄像头未接**
   （`/dev/video0: No such file or directory`，
   `dmesg: sunxi:vin:[ERR]: registering imx219, No such device!`），
   `frames=hub:unconfigured`，所以无法产生一张真实的图供回收。已做的是：
   临时 drop-in 把上限调到 250000、重启、确认 `quota=250000` 生效（即配置确实
   被读进生产路径）、然后**完整回滚**（drop-in 删除、`daemon-reload`、重启、
   `WEAKNET_SNAPSHOT_MAX_BYTES=2147483648` 复原、3 张真实 JPEG 382184 字节
   原封不动、维护标记最后才摘、临时目录与 /tmp 测试二进制清理干净）。
   **能说的是**：接线由变异验证过的单元/集成测试保证，二进制在真机上跑通；
   **不能说的是**「一次真实事故触发了真实回收」。
2. **云端保留期与孤儿清扫都只在 SQLite 上验证过「删得对不对」。** 保留期的
   真实 Postgres dry-run/apply 在前序会话对隔离探针租户做过；孤儿清扫器**没有**
   在真实 Postgres 上跑过。它的查询很简单（一次 `IN`，分片 500），但这是
   推断，不是证据。
3. **两个 systemd timer 都没有在生产环境安装过。** 单元文件与包装脚本写好了、
   包装脚本的控制流有 19 项测试，但 `systemctl enable --now` 没执行过。
4. **`getSnapshotImageObjectUrl` 没有浏览器端验证。** 类型与构建通过，实际
   图片渲染、blob URL 回收、500 时的错误文案都没有在真实浏览器里点过。

### 顺带发现、故意没修

- **生产 Postgres 的 RLS 实际失效**：`weaknet` 角色是 `rolsuper=t |
  rolbypassrls=t`；187/189 张表启用了 RLS，但该角色绕过它。租户隔离完全靠
  显式 `WHERE tenant_id = ...`。这是真实的安全问题，但超出本次范围，而且改生产
  库权限是部署层决定，已另行报告。

### 后续是否值得升格为正式 OpenSpec change

值得，但不是现在。上面第 1~4 条边界里有三条依赖**外部条件**（摄像头接上、
生产 Postgres 可用、生产机允许装 timer），不是补代码能解决的。等摄像头恢复
后，建议走一次正式 change 把「真实事故 → 真实回收 → 真实孤儿清扫 → 前端
渲染」端到端钉一遍证据，那时才谈得上「留证链路已验证」。
