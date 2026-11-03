# 日志系统补全（滚动 / 实时级别 / 运行时调试 / libbpf 归因 / 元信息）

> 日期：2026-11-02
> 分类：工作流外实施（用户明确指定"不用走工作流，用路线 A"）
> 结果：已完成

## 构思

### 要解决的问题

对全仓日志做一次审计，发现的问题按严重度分三档：

**P0（会导致现场失去排障依据）**

1. **文件日志永不滚动、永不清理**。`startFileLog()` 打开
   `server_YYYYMMDD_HHMMSS.log` 后一直追加，`cleanOldLogs(dir, 7)` 只在
   启动时和每 24 小时跑一次按**年龄**清理。板端是长期在线的嵌入式设备，
   单进程跑几周 → 单文件无上限增长；而 `cleanOldLogs` 只删 7 天前**整文件**，
   对"今天这个 8 GB 的文件"完全无效。同时 `openFileLog` 用
   `ios::out|ios::trunc`，同一秒内重启会截断上一个进程的日志。

2. **`server.log_level` 运行期设置是假成功**。`weaknet-cli set server.log_level`
   走 `setMonitorParam` → `applyMonitorParam`，只把字符串写进
   `cfg.log_level`，**从不调用 `Logger::setLogLevel`**。命令返回 ok、配置里
   值也变了，但 glog 级别纹丝不动。更糟的是写入前不做校验，
   `set server.log_level verbose` 同样返回 ok。

3. **eBPF 加载失败丢失根因**。14 处 `bpf_object__open*/load` 调用点里，
   失败分支普遍只写一行 `failed to load BPF object`。libbpf 的真正原因
   （`invalid mem access 'scalar'`、`map create: Operation not permitted`、
   校验器逐条指令日志）走默认 print 回调直写 stderr。`net_traffic.cpp`
   更把它写成一个 `vfprintf(stderr, ...)` 的 lambda，与项目日志通道完全脱节。
   走 journald 时原始错误和被截断的监控器日志对不上，无法归因。

4. **`LOG_DEBUG` 是死代码**。宏体被 `#ifdef DEBUG` 守卫，但两种文档化构建
   （`-DCMAKE_BUILD_TYPE=Debug`）**既不定义 `DEBUG` 也不定义 `NDEBUG`**
   （实测 `compile_commands.json` 确认）。于是 16 处调试日志全部展开为空语句，
   从来没有输出过。

**P1（可用性 / 可维护性）**

5. **模块标签名不副实**。17 个 eBPF 监控器里，dns / http_latency /
   skb_drop / wifi_loss / process_profiler / tcp_connect 全打 `[network]`，
   tcp_retrans / tcp_conn / tcp_rtt 全打 `[tcp_loss]`。`grep '\[dns\]'`
   搜不到 DNS 的任何一条日志，而这些标签本应与 `weaknet-cli get <name>`
   用同一个词。

6. **没有启动版本元信息**。日志里看不出当前跑的是哪个版本、什么构建类型、
   pid 多少。OTA 已引入多版本目录后，这一点让"日志到底是哪次部署写的"
   变成靠目录名反推。

7. **没有崩溃调用栈**。glog 自带
   `InstallFailureSignalHandler()`，但从未被调用；SIGSEGV 只能看到进程消失。

**P2（卫生）**

11. 零调用宏 `LOG_PERF` / `LOG_NETWORK_STATE` / `LOG_EVENT` /
    `LOG_ERROR_WITH_CODE` / `LOG_IF_INFO` / `LOG_IF_ERROR`。
12. 孤儿 API：`setLogDir` / `requestStop` / `isInitialized` /
    `getCurrentTimestamp` / `isFileLogActive`。
13. `Logger::signalHandler` 头注释写"清理日志文件"，实际只置原子标志；
    正文注释把它当成 SDK 的 handler。
14. 退出路径裸调 `google::ShutdownGoogleLogging()`，绕过 `Logger::shutdown()`，
    日志文件里看不到"服务正常退出"，与崩溃无法区分。
16. logger 测试只覆盖 5 项（两个格式化宏 + 磁盘保护 + 两个清理用例），
    上面每一条修复都没有对应断言。

### 为什么不做进 OpenSpec change

用户两次明确指示"不用走工作流，用路线 a"。另有一条独立理由：本次改动
主要是**把已有接口的表现修成它本来承诺的样子**（滚动、实时生效、日志级别），
不是新增产品表面；走 change 流程反而会因为 `planning_fingerprint` 与
`task_verify.sh` 的 evidence 记录把"改 logger 内部"变成一次重型仪式。

### 备选方案与取舍

- **滚动**：考虑过启用 glog 自己的 `FLAGS_max_log_size` + `FLAGS_log_dir`。
  实测证伪：`FLAGS_logtostderr=true` 时 glog **一个文件都不写**，
  `FLAGS_max_log_size` 完全不起作用。落盘只能由项目自己的 sink 负责，
  滚动也必须在 sink 里实现。
- **`LOG_DEBUG` 门控**：考虑过改成 `#ifndef NDEBUG`。同样无效——两种构建
  都不定义 `NDEBUG`。最终选**运行期原子开关**，由 `server.log_level=debug`
  驱动，顺带把"运行期调级别"这件事一次性做对。
- **日志级别生效点**：候选是 `dbus_service.cpp` 的本地写路径。放弃，因为
  云端下发（`edge_telemetry_exporter.cpp:897/901`）和事务回滚
  （`weaknet_config.cpp:1221`）也走同一批 key，各改一处必然漂移。
  最终收口在 `setMonitorParam()`——三条写路径的唯一汇合点。
- **日志目录**：历史实现 glog 用 `./logs/server`、文件 sink 用
  `./server/log`，是两个不同目录，且都相对 `WorkingDirectory`
  （= `current/server`）解析 → 每次 OTA 升级连同旧版本目录一起被替换。
  改为 `$WEAKNET_LOG_DIR` → `$WEAKNET_ROOT/data/logs` → `./logs/server`
  的三级解析，让日志落到 `data/`（部署时持久化、不随版本切换）。
- **libbpf 桥接的落点**：考虑过塞进 `EbpfMonitorStateSupport`（每个监控器
  都持有的公共成员）。放弃，因为 `libbpf_set_print` 是**进程级全局** API，
  与"每个监控器一个状态对象"的生命周期模型不匹配。独立成
  `ebpf_log_bridge.*`，由各加载点调用幂等的 `installLibbpfLogBridge()`。

## 实现方式

### 文件清单

新增：

| 文件 | 作用 |
|---|---|
| `server/include/ebpf_log_bridge.hpp` | libbpf → 日志桥接接口 |
| `server/src/ebpf_log_bridge.cpp` | 回调实现（按行拆分 + 分级 + 线程局部缓存） |

核心改写：

| 文件 | 改动 |
|---|---|
| `server/include/logger.hpp` | 增 `LogLevel::DEBUG`、`parseLogLevel`/`applyLogLevel`、10 个监控器模块常量、`installCrashHandler`、滚动常量；删 4 个孤儿 API 与 6 个零调用宏；`isFileLogActive`/`writeToFileLog` 降为 private + `friend class FileLogSink` |
| `server/src/logger.cpp` | 滚动（1 MiB）+ 裁剪（5 个）+ 不覆盖旧会话 + 启动元信息 + 崩溃 handler + 运行期级别 + 收尾日志 |
| `server/src/server.cpp` | 日志目录三级解析；`applyLogLevel` 取代手工 `parseLogLevel`；`Logger::shutdown()` 取代裸 `ShutdownGoogleLogging()` |
| `server/src/weaknet_config.cpp` | `server.log_level` 写入前校验；`setMonitorParam()` 内单点应用 |
| `server/CMakeLists.txt` | 登记新源文件；给 `weaknet_common` + `server_lib` 注入 `WEAKNET_VERSION` / `WEAKNET_BUILD_TYPE` |
| 9 个 eBPF 监控器 + 3 个蓝牙分析器 + `net_traffic.cpp` + `monitor_ebpf_plugins.cpp` | 接桥接、把 `takeLibbpfError()` 拼进失败日志、重打模块标签 |

### 关键决策

**1. 文件日志滚动必须自己实现。**
`kFileLogMaxBytes = 1 MiB`，`kFileLogMaxFiles = 5`，最坏占用约 5 MiB 有界。
滚动在 `writeToFileLog()` 内、持 `file_mutex_` 时进行；裁剪按 mtime 排序删最旧，
且**永不删除当前打开的文件**。文件名同秒冲突时追加 `_2/_3…`，
不再用 `trunc` 截断上一会话。

**2. 锁序是这里最容易踩死锁的地方。**
glog 在回调 `send()` 期间持有自己的 sink 锁。若 `startFileLog()` 持着
`file_mutex_` 去调 `AddLogSink`，就形成
`file_mutex_ → glog sink 锁` 与
`glog sink 锁 → send() → writeToFileLog → file_mutex_` 的 ABBA 死锁。
因此文件准备与 `AddLogSink` 必须在**两段独立作用域**里做，
文件头只在第一段（持锁）内写，`AddLogSink`/`RemoveLogSink` 一律在锁外。

**3. `LOG_DEBUG` 用运行期原子而非编译期开关。**
`debug_enabled_` 是 `std::atomic<bool>`，关闭时只有一次 relaxed load，
消息流**不会被求值**（有测试断言这点：
`DebugMacroDoesNotEvaluateMessageWhenDisabled`）。传给 glog 的级别被夹到
INFO，不动 glog 自己的 verbosity 机制。

**4. libbpf 桥接必须按行拆分 + 两遍 vsnprintf。**
libbpf 把整段校验器日志放在**一次** print 调用里
（`"libbpf: prog 'x': -- BEGIN PROG LOAD LOG --\n%s-- END PROG LOAD LOG --\n"`），
单次上千行、上百 KB。当一行处理必然截断成垃圾，栈缓冲区也装不下，
所以先 `vsnprintf(nullptr, 0, ...)` 取长度，再 `va_copy` 渲染。
分级策略：以 `libbpf:` 开头的结论行按级别进 WARNING/INFO 并缓存；
其余校验器原文只在 debug 打开时逐行降级输出（上限 512 行）。
失败处 `takeLibbpfError()` 取走缓存并拼进自己的失败日志 —— **不开 debug
也能看到根因**，实测输出：

```
[ERROR] [dns] DnsMonitor: failed to load BPF object
        (libbpf: loading /tmp/fake.bpf.o | elf: '/tmp/fake.bpf.o' is not a proper ELF object)
```

缓存用 `thread_local`：open/load 是同步调用，失败原因必然属于当前线程；
全局缓存会在多监控器并发初始化时互相覆盖。

**5. 版本元信息要写两次。**
`init()` 输出的那行元信息发生在 `startFileLog()` **之前**，sink 还没挂上，
文件里收不到。而文件日志正是离线取证的那条通道，恰恰最需要版本信息。
所以 `startFileLog()` 在文件头里再写一遍
`[system] weaknet version=… build=… pid=…`。

**6. 模块标签与配置名对齐。**
新增 `DNS`/`HTTP`/`SKB_DROP`/`WIFI_LOSS`/`PROCESS_PROF`/`TRAFFIC`/
`TCP_CONNECT`/`TCP_CONN`/`TCP_RETRANS`/`TCP_RTT`/`EBPF`，
重打 9 个监控器共 94 处标签 + `monitor_ebpf_plugins.cpp` 13 处。
约定写进头文件：标签与 `weaknet_config` 的监控器名是同一个词。

**7. 孤儿接口按"有没有真实生产消费者"逐个判定，不搞一刀切。**
- `setLogDir` / `requestStop` / `isInitialized` / `getCurrentTimestamp`
  → 零调用，直接删。
- 6 个零调用宏 → 直接删。
- `isFileLogActive` / `writeToFileLog` → 有唯一消费者 `FileLogSink`，
  但它在同一个 `.cpp` 内。**收进 private + friend**，而不是删掉（sink 需要）
  也不是留在 public（外部不该手工拼日志行）。
  对应地，测试里 7 处对内部标志的断言全部改成**基于行为的断言**
  （"磁盘上真的出现了标记行"、"文件数不超过上限"），本来就该这么写。

**8. 文档同步（铁律要求）。**
代码与文档相左时必须同步改：
- `docs/架构设计-模块与代码结构.md`：新增「5. 日志系统」整节（通道分工、
  目录解析、模块标签、运行期级别、启动元信息、崩溃现场、libbpf 桥接），
  更新技术栈表与目录/头文件清单。
- `docs/交叉编译与开发板部署.md`：8 处日志路径从 `server/logs/server`
  改为 `<root>/data/logs`；删掉失去意义的 `--exclude 'server/logs/'`；
  开发板目录树改成 OTA 布局；测试脚本补 `WEAKNET_ROOT`。
- `docs/weaknet_cli_usage.md` + `config.yaml`：`server.log_level`
  枚举补 `debug`。

**9. 刻意撤销的一处改动。**
中途给 `server.cpp` 加过 `--log-level` CLI 参数。识别出这是**未规划的新
生产表面**（违反孤儿接口铁律：没有规划的需求 + 没有真实消费者 + 没有可观测
命令），在扩散前删掉，改由配置文件 / `weaknet-cli set` 两条既有通道驱动。

### 验证手段

1. **x86 全量测试**：`ctest --test-dir build-x86/server --output-on-failure`
   → **55/55 通过**（基线也是 55 套件，未增删套件，只增用例）。
   `test_logger_gtest` 从 5 项扩到 **20 项**；
   `test_weaknet_config_gtest` 增 4 项 log_level 用例（60 项全通过）。
2. **ARM64 交叉编译**：`cmake -B build-arm64-native
   -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-arm64-cross.cmake
   -DCMAKE_BUILD_TYPE=Debug -DBUILD_EBPF=ON && cmake --build ... -j$(nproc)`
   → **rc=0，无新增 warning**（现存 `LIBBPF_OPTS` variadic 告警为改动前既有）。
   `strings` 确认 ARM64 二进制内嵌 `1.1.0` / `Debug`。
3. **真实进程冒烟**：`WEAKNET_ROOT=/tmp/wnsmoke2 ./weaknet-dbus-server`
   → 日志确实落到 `/tmp/wnsmoke2/data/logs/`，文件头含版本与 pid。
4. **运行时开关端到端探针**：临时程序验证
   `applyLogLevel("debug")` 后 `LOG_DEBUG` 出现、切回 `info` 后消失；
   关闭状态下消息**未被求值**。
5. **libbpf 桥接端到端探针**：把非 ELF 文件喂给 `bpf_object__open`，
   确认 libbpf 结论进了 WARNING 通道，且 `takeLibbpfError()` 把它拼进了
   监控器的 ERROR 行。
   （三个临时探针用完即删，未进仓库——符合"一次性探针不得留在工作区"。）

## 结果

### 最终状态

七项 P0/P1 修复全部落地并验证：

1. 文件日志 1 MiB 滚动 + 5 个上限 + 不覆盖旧会话 → 占用有界 `~5 MiB`。
2. `server.log_level` 三条写路径（本地 D-Bus / 云端下发 / 事务回滚）
   实时生效；非法值在写入前被拒。
3. libbpf 加载失败保留根因，14 个加载点全部接线。
4. `LOG_DEBUG` 由 `server.log_level=debug` 运行期驱动。
5. 9 个监控器 + 插件层共 107 处模块标签与配置名对齐。
6. 启动元信息含版本/构建类型/pid，且**同时**进 stderr 与文件日志。
7. `installCrashHandler()` 在其它线程创建前安装。

P2 卫生项：零调用宏与孤儿 API 已删；内部入口收进 private；
退出路径统一走 `Logger::shutdown()`；logger 测试 5 → 20 项。

### 遗留问题

- **崩溃处理器在 ARM64 上未做真机崩溃验证**。已确认 sysroot 的 glog 0.4.0
  暴露 `InstallFailureSignalHandler()` 且 ARM64 编译通过，但"真的 SIGSEGV
  能打出符号化调用栈"需要板端实跑。符号化质量取决于板端是否有对应
  调试符号，这一点本次未验证。
- **`FLAGS_max_log_size` 保留但无效**。代码里设了 10（MiB），注释已写明
  在 `logtostderr=true` 下不生效；保留是为了将来若关掉 stderr 输出仍有护栏。
  属于"已知无效但刻意保留"，不是遗漏。
- **`weaknet-test-full.sh` 只补了 `WEAKNET_ROOT`**，没有把板端日志
  纳入自动断言（仍靠 `/tmp/weaknet-server.log` 看 stderr）。
  若要断言文件日志，需要脚本知道 `data/logs` 路径并解析最新文件——
  属于测试脚本增强，本次未做。

### 是否值得升格为正式 OpenSpec change

不建议。本次是"把既有承诺修成真的"，没有新增产品表面；
走 change 流程的代价（fingerprint 冻结、evidence 记录、三方评审）
与收益不匹配。若将来要**新增**日志能力（例如日志按模块分文件、
或把日志回传给云端），那属于新表面，应当走完整工作流。

### 部署注意

本次改动**未提交、未部署**。部署时注意：
- 板端日志路径变为 `/home/radxa/weaknet/data/logs/`；
- `data/logs` 由服务以 root 创建，`radxa` 用户读取需 sudo
  （与 `history.db` 同样的权限模型）；
- 旧的 `current/server/logs/`、`current/server/log` 会被 OTA 切版本
  自然丢弃，无需手工清理。
