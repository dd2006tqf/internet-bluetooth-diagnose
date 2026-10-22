# WeakNet OTA 固件更新与安全发布指南

> **版本**：v1.3 (涵盖 Phase 1 本地闭环 + Phase 2 ECDSA 签名链 + Phase 3 远程拉包与空间 GC + P0-A 升级事务状态机)  
> **适用平台**：Radxa Cubie A7A (ARM64) / x86_64 宿主机交叉编译环境  
> **核心特性**：A/B 软链接原子翻转、ECDSA NIST P-256 强制验签、180s 健康看门狗、故障自动回滚、状态与配置隔离保护、过旧版本自动 GC、**升级事务持久化状态机（崩溃点可判定）**

---

## 1. 架构总览

WeakNet 的 OTA 升级采用轻量级软件双槽位（Slot）设计，避免在嵌入式系统上对 Flash 重新分区的巨大风险。

```text
开发板 /home/radxa/weaknet/
├── releases/                     # 多版本隔离目录
│   ├── legacy-flat/              # 首次部署自动归档的旧扁平布局
│   ├── 1.0.0/                    # 历史稳定版（保留供回滚）
│   └── 1.1.0/                    # 当前激活版本
│       ├── server/               # 服务端二进制与 eBPF 对象 (*.bpf.o)
│       ├── client/               # 客户端工具 (weaknet-cli 等)
│       ├── lib/                  # 运行时动态库 (libbpf.so 等)
│       ├── VERSION               # 本版本号 (1.1.0)
│       ├── manifest.sha256       # 全量文件 SHA256 校验清单
│       └── .committed            # 健康窗口通过后的“转正标记”
├── current -> releases/1.1.0     # 【原子翻转点】基于 Linux rename 原语切换
├── server -> current/server      # 兼容软链，历史命令与路径 100% 保持有效
├── client -> current/client
├── lib -> current/lib
├── weaknet-local-update.sh       # 本地升级执行体（随包下发并自动自更新）
├── data/                         # 【绝对只读隔离】5 个核心状态文件（history.db、epoch 等）升级永不触碰
└── config/                       # 【绝对只读隔离】release_pub.pem 与 config.yaml

系统路径（不随 OTA 制品变化）：
├── /usr/local/lib/weaknet/weaknet-ota-ctl   # 升级事务控制器（P0-A，唯一的指针/状态写入口）
└── /var/lib/weaknet-ota/transaction.json    # 事务状态（schema v1，root 独占 0600）
```

### 1.1 为什么需要事务控制器（P0-A）

**升级机制的可靠性不能完全依赖被升级的 weaknet-server 本身。**

原实现里 `current` 指针、`.previous` 回滚指针、`.committed` 转正标记由
`cmd_apply` / `cmd_rollback` / `cmd_adopt` / legacy 迁移 / `--commit` 多条
执行路径各自读写。一旦升级进程在关键窗口被掉电或杀死，**没有任何一方知道
这次升级做到哪一步**：指针翻了但没转正，还是转正了但投影没写？无从判断。

P0-A 把三件事收口到一处：

| 收口项 | 权威位置 | 说明 |
|--------|---------|------|
| 版本指针 `current` | 控制器独占写 | 升级脚本不再直接改软链 |
| 回滚指针 `.previous` | 控制器派生 | 语义固定为「上一稳定版本」 |
| 转正标记 `.committed` | 控制器派生 | C++ `update_status.cpp` 仍读它（兼容层） |
| 事务状态 | `/var/lib/weaknet-ota/transaction.json` | 唯一的持久化事实来源 |

控制器装在 `$ROOT` **之外**：事务 schema 由它写入，若它随版本升级，旧版恢复
服务读到的就是新版 schema，语义对不上。

### 1.2 事务阶段与状态转换

```text
                    begin
                     │
                     ▼
               ┌───────────┐
               │ PREPARED  │  目标目录已落位，指针未动
               └─────┬─────┘
             arm-switch│
                     ▼
           ┌────────────────┐
           │ SWITCH_PENDING │  切换意图已落盘，指针可能未翻
           └────┬───────────┘
        do-switch│
                 ▼
            ┌────────┐
            │ TRIAL  │  指针已翻，等待健康窗口
            └───┬────┘
       health OK │        health 失败
        commit   │         rollback-arm
                 ▼               ▼
        ┌────────────┐   ┌────────────────┐
        │  COMMITTED │   │ ROLLBACK_PENDING│
        └────────────┘   └────────┬───────┘
                                   │ rollback-do
                                   ▼
                          ┌────────────────┐
                          │ ROLLBACK_VERIFY│
                          └────────┬───────┘
                                   │ rolled-back
                                   ▼
                          ┌──────────────┐
                          │ ROLLED_BACK  │
                          └──────────────┘

  终态：COMMITTED / FORCED_COMMITTED / ABORTED / ROLLED_BACK
  任意非终态 → RECOVERY_REQUIRED（停止一切自动动作，等人工介入）
```

| 阶段 | 含义 | 崩溃后该做什么 |
|------|------|--------------|
| `PREPARED` | 目录就位，指针未动 | 保守取消（abort） |
| `SWITCH_PENDING` | 意图已落盘，指针未翻 | 保守取消（abort） |
| `SWITCH_PENDING` + 指针已翻 | 翻链完成但状态未推进 | 推进到 `TRIAL` |
| `TRIAL` | 试运行中 | 继续健康验证 |
| `ROLLBACK_PENDING` | 回滚意图已落盘 | 继续回滚 |
| `ROLLBACK_VERIFY` | 指针已回退，待验证旧版 | 继续验证旧版健康 |
| `RECOVERY_REQUIRED` | 状态不可信 | **禁止任何自动动作** |

判定入口（只读，不改任何东西）：

```bash
sudo python3 /usr/local/lib/weaknet/weaknet-ota-ctl \
    --root /home/radxa/weaknet --state-dir /var/lib/weaknet-ota \
    reconcile --dry-run
```

输出形如 `{"action": "abort|mark_trial|continue_trial|continue_rollback|project_views|recovery_required", ...}`。

> **P0-A 的边界**：本阶段只做到「可观察、可判定、可显式修复」。
> 开机自动恢复由 **P0-B**（§1.4）补齐。

### 1.4 开机自动恢复服务（P0-B）

`weaknet-ota-recover` 是一个**独立于 weaknet-server** 的 systemd 服务
（`weaknet-ota-recover.service`），开机自启。它把 P0-A 的「判定」变成
「执行」。

```text
系统路径（不随 OTA 制品变化）：
├── /usr/local/lib/weaknet/weaknet-ota-recover      # 恢复器本体
├── /usr/local/lib/weaknet/weaknet-ota-ctl          # 事务控制器（唯一写入口）
└── /var/lib/weaknet-ota/
    ├── transaction.json                            # 权威事务状态
    ├── history/                                    # 归档的终态事务
    └── RESCUE.md                                   # 救援模式现场报告（仅救援时生成）
```

**三条硬约束与对应设计：**

| 约束 | 设计 |
|------|------|
| 即使升级脚本消失也能读取事务 | 恢复器**从不引用** `weaknet-local-update.sh`（源码级门禁），只读 `/var/lib/weaknet-ota/transaction.json` |
| 即使 weaknet-server 无法启动也能完成回滚决策 | 决策是纯函数（`decide()`），不依赖服务在线；健康判据里 `systemctl is-active` 是硬门禁，D-Bus 不可达**不算健康但允许继续重试**；`systemctl show NRestarts` 抓崩溃循环作为硬证据 |
| 即使回滚失败也能保留 SSH 救援入口 | 救援模式：停服 + mask（不可用时退到 disable）+ 保证 `ssh` / `weaknet-tunnel` active + 写 `RESCUE.md`；**不做版本切换、不做无限重启、不启动 weaknet-server** |

**尝试次数是持久化的**：`boot_attempts` 存在事务状态里，**重启后不归零**。
断电重来的无限重试必须被它拦住。计数按内核 `boot_id` 幂等——恢复服务自身
被 systemd 重启不算一次新尝试。

**救援模式的行为约定：**

- 不自动翻 `current`（未知状态不能继续传播）
- `weaknet-server` 已 stop + mask/disable（systemd 不会拉起它）
- 事务状态、失败日志、历史版本目录**全部保留**（那是排查现场的唯一凭据）
- `RESCUE.md` 记录现场信息与人工恢复步骤

**人工退出救援模式：**

```bash
sudo systemctl unmask weaknet-server
sudo systemctl enable weaknet-server
sudo python3 /usr/local/lib/weaknet/weaknet-ota-ctl \
    --root /home/radxa/weaknet --state-dir /var/lib/weaknet-ota \
    recover --accept <known-good-version>
sudo systemctl restart weaknet-server
```

**当前边界**：P0-B 提供的是**开机自动恢复 + 最小 SSH 救援**。
**真正的断电自愈尚未验证**——`fsync` 保证的是「内核 → 设备」，不保证设备
写缓存已落到介质；板端根分区是 `/dev/sda3`（SATA/USB）。所有验证都是用
状态构造与 `kill` 代替的，**不等于掉电验证**。

### 1.3 关键不变量

- **失败候选绝不成为回滚目标**：`begin` 拒绝 `--rollback-target == --to`
  （同版本幂等重装除外），`rolled-back` 也不会把失败版本写进
  `previous_stable_version`。
- **失败候选绝不成为 previous**：自动回滚后 `previous` 仍是回滚**之前**的
  稳定版本。
- **同版本重装不把 previous 定成 current 自己**：否则之后的无参回滚会以
  「目标与当前相同」失败，设备少一条自动退路。
- **指针写入必须原子**：软链用 `symlink + rename + fsync(dir)`，状态文件用
  `temp + fsync + rename + fsync(dir)`。只 `rename` 不够，目录项也必须落盘。
- **同版本重装用 `renameat2(RENAME_EXCHANGE)` 原子交换**：先挪走再挪进会在
  `current` 指向的目录上留下缺失窗口，此时掉电即砖。文件系统不支持时
  **拒绝升级**，不退化成两步 rename。
- **fail-closed**：schema 不认识、phase 不认识、指针指向不在版本集合内的值、
  JSON 损坏 —— 一律不做破坏性动作。
- **单一写者**：所有状态变更走同一把 `flock` 互斥锁，冲突立即失败不排队。
- **推断来源必须可审计**：`stable_version` / `previous_stable_version` 各带
  一个 `source` 标记（`current` / `transaction_history` / `committed_mtime` /
  `previous_file` / `manual` / `unknown`）。人工回滚之后，"版本号最高的已
  转正版本"不等于"上一稳定版本"——**只有事务历史能区分**，历史不足时
  明确返回未知，不猜。

---

## 2. 安全与防护机制

1. **ECDSA (NIST P-256) 数字签名**：
   - 发布方使用私钥生成伴随签名文件 `weaknet-<version>-arm64.tar.zst.sig`；
   - 板端内置公钥 `/home/radxa/weaknet/config/release_pub.pem`；
   - **生产态默认 fail-closed**：未签名、私钥不匹配、或固件包被篡改 1 字节，一律在解包第一步直接拒绝（仅 `--insecure` 显式标志可在调试时跳过）。
2. **清单逐文件校验**：每个版本包含 `manifest.sha256`，解包后逐文件比对校验。
3. **单调版本策略**：内置数字版本比较，默认禁止降级（`target >= current`），拦截版本回滚攻击（显式 `--allow-downgrade` 方可降级）。
4. **180s 健康看门狗与自动回滚**：
   - 翻转 `current` 软链接并重启 `weaknet-server` 后，进入健康观察期；
   - 需连续 3 次探测全绿：
     1. 服务进程处于 `active`；
     2. D-Bus 接口 `HealthCheck` 可达；
     3. eBPF 探针加载 `GetEbpfMonitorHealth` 正常；
     4. `GetUpdateStatus.installed_version == 目标版本`（防假绿灯）；
   - **全绿**：写入 `.committed` 转正，清理旧副本；
   - **超时或崩溃**：**自动切换回上一版本软链接并重启**，保证无人值守设备不死机。
5. **历史版本自动垃圾回收 (GC)**：
   - 每次升级转正后自动清理历史残留的 `.staging.*` 临时目录；
   - 版本回收委托给事务控制器，**只在事务处于终态时运行**——事务未完成就
     回收等于删掉回滚证据。保护集合是 `current` / `stable` / `previous` /
     当前事务引用的全部版本，这些无论多少都留着；
   - `max_keep`（默认 3）指**额外保留的未受保护版本数**，所以磁盘上的版本
     总数可能超过它——那是刻意的：误删回滚目标的代价远大于多占几 MB。
6. **升级事务持久化（P0-A）**：
   - 状态文件 `/var/lib/weaknet-ota/transaction.json`（0600，仅 root 可写）
     是唯一的事实来源，`current` / `.previous` / `.committed` 都是它的投影；
   - 落盘顺序保证「先落意图、再动指针」，所以任何崩溃点都能从
     `(事务阶段, current 真实指向)` 推出唯一且非破坏性的下一步；
   - 详见 §1.1–§1.3。

---

## 3. 运维操作手册（Agent 指南）

### 3.0 事务状态查询与人工修复（P0-A）

```bash
# 1. 事务权威状态（JSON）
sudo python3 /usr/local/lib/weaknet/weaknet-ota-ctl \
    --root /home/radxa/weaknet --state-dir /var/lib/weaknet-ota status

# 2. 崩溃点判定（只读，绝不改任何东西）
sudo python3 /usr/local/lib/weaknet/weaknet-ota-ctl \
    --root /home/radxa/weaknet --state-dir /var/lib/weaknet-ota reconcile --dry-run

# 3. 升级脚本的汇总视图（人读）
sudo /home/radxa/weaknet/weaknet-local-update.sh status
```

`reconcile --dry-run` 返回的 `action` 取值与处置：

| action | 含义 | 处置 |
|--------|------|------|
| `none` | 无进行中事务 / 事务已结束 | 无需动作 |
| `abort` | 准备或切换意图阶段，指针未翻 | 保守取消事务 |
| `mark_trial` | 指针已翻但状态未推进 | 推进到 TRIAL |
| `continue_trial` | 试运行中 | 继续健康验证 |
| `continue_rollback` | 回滚未完成 | 继续回滚 |
| `project_views` | 终态但兼容投影滞后 | 补写 `.committed` |
| `recovery_required` | 状态不可信 | **停止一切自动动作**，人工介入 |

人工退出 `RECOVERY_REQUIRED`（必须显式选择，不允许"自动恢复"）：

```bash
# 认定某个版本为稳定版本并切过去
sudo python3 /usr/local/lib/weaknet/weaknet-ota-ctl \
    --root /home/radxa/weaknet --state-dir /var/lib/weaknet-ota \
    recover --accept 1.1.0

# 或显式放弃本事务
sudo python3 /usr/local/lib/weaknet/weaknet-ota-ctl \
    --root /home/radxa/weaknet --state-dir /var/lib/weaknet-ota recover --abort
```

> `reconcile` 判定出 `continue_trial` / `continue_rollback` 时，仍需要健康
> 验证才能推进，而健康验证需要 `weaknet-server` 在线。这正是 **P0-B**
> （独立 systemd 恢复服务）要解决的问题——恢复服务不能依赖被恢复的服务本身。

### 3.1 查询设备 OTA 状态
```bash
# 在开发板运行（普通用户即可，走 D-Bus 只读方法）
weaknet-cli update status
# 输出示例：
# {"installed_version":"1.0.0","layout":"releases","pending_staged_version":"","current_committed":true}
```

### 3.2 本地固件包升级
```bash
# 校验固件包合法性与签名（只读，无需 root）
weaknet-cli update verify /path/to/weaknet-1.1.0-arm64.tar.zst

# 执行升级（需 sudo，自动执行“解包→验签→翻转→重启→健康观察→转正/回滚”）
sudo weaknet-cli update apply /path/to/weaknet-1.1.0-arm64.tar.zst

# 应急操作：跳过健康窗口直接转正（不建议生产使用）
sudo weaknet-cli update apply /path/to/pkg.tar.zst --commit

# 降级安装
sudo weaknet-cli update apply /path/to/pkg.tar.zst --allow-downgrade
```

### 3.3 远程 URL 一键在线升级（Phase 3 闭环）
无需手动下载传输，升级脚本内置下载器，会自动下载固件及对应的 `.sig` 签名文件：
```bash
sudo weaknet-cli update apply https://github.com/dd2006tqf/AI-powered-Network-Diagnostics/releases/download/v1.1.0/weaknet-1.1.0-arm64.tar.zst
```

### 3.4 手动回滚至上一版本
```bash
sudo weaknet-cli update rollback
```

### 3.5 云端下发 OTA 升级（pending_actions 通道）

平台侧可经既有下行动作通道远程触发升级，无需登录开发板。板端把
`system.ota_upgrade_url` 纳入配置白名单（与云端 `CONFIG_KEYS` 逐位对齐，
由 12 位金标 `catalog_version` 跨语言测试守护），收到后**异步**调度升级子进程，
不阻塞遥测/评估主循环。

**端到端链路：**

```
平台排队动作 (config_key=system.ota_upgrade_url, value=固件 URL)
  → 遥测响应携带 pending_actions（Ed25519 签名 + generation 防重放）
  → 板端 exporter 校验签名/防重放 → fork+setsid 执行 weaknet-local-update.sh apply <URL>
  → 脚本迁出服务 cgroup → 下载+验签+落位+翻转+健康窗口
  → 脚本把终态写入 data/pending_ota_receipt.json（result: SUCCEEDED/FAILED）
  → exporter 轮询拾取终态 → queueActionResult 回传 /edge/action-results
```

**回执生命周期（两段式，防"假 APPLIED"）：**

| 阶段 | result 值 | 写入者 |
|------|-----------|--------|
| 调度成功（fork 已启动） | `IN_PROGRESS` | exporter（scheduleOtaUpgrade） |
| 升级成功转正（`.committed` 落盘） | `SUCCEEDED` | **升级脚本显式写入**（唯一成功出口） |
| 升级失败/健康窗口回滚/脚本被杀 | `FAILED` | 升级脚本 EXIT 陷阱（不看退出码） |
| 脚本超时未定稿（>600s stall） | —（按 FAILED 上报） | exporter 超时兜底 |

关键约束（真机踩过的坑，勿回退）：
- **绝不凭"重启发生了/退出码 0"推断升级成功**——bash 死于 SIGTERM 时 EXIT
  陷阱里 `$?` 是上一条命令的状态（实测为 0），据此上报会把"服务重启把升级
  脚本连带杀死"误报成升级成功。`SUCCEEDED` 只能由脚本在 `.committed` 之后
  显式写入。
- **升级脚本必须先迁出服务 cgroup**（写 `/sys/fs/cgroup/cgroup.procs`）：
  脚本由服务 fork 而来，`setsid` 只脱离会话不脱离 cgroup；不迁出则
  `systemctl stop weaknet-server`（KillMode=control-group）会把升级进程一起
  SIGTERM，死在翻转之前。
- **release 目录必须做属主/权限归一化**（`normalize_release_perms`）：服务
  进程受 unit `CapabilityBoundingSet` 限制（无 CAP_CHOWN），tar 改不了属主；
  归档根目录权限又随构建机 umask 漂移（踩过 700）——两层叠加会把 radxa
  锁在 `releases/<ver>` 外（`weaknet-cli` Permission denied）。chown 尽力
  而为，chmod 755/o=rX 必做。
- 回执文件 `data/pending_ota_receipt.json` 在调度时落盘（含 action_id/
  claim_token/generation/target_url/result），服务重启不丢；exporter 在
  `start()` 与主循环（≤5s 周期）双路轮询。
- 升级子进程 stdout/stderr 追加写入 `data/ota_upgrade.log`，真机故障先看这里。

**平台侧下发示例（rehearsal 环境直插 DB）：**

```python
from datetime import UTC, datetime
from uuid import uuid4
from sqlalchemy import create_engine, text
eng = create_engine("postgresql+psycopg://weaknet:<pw>@127.0.0.1:5432/industrial_ops")
with eng.begin() as c:
    max_gen = c.execute(text("SELECT coalesce(max(generation),0) FROM network_pending_actions"
                             " WHERE tenant_id=:t AND asset_id=:a"),
                        {"t": "tenant-m1-demo", "a": "radxa-cubie-a7a"}).scalar_one()
    c.execute(text("""INSERT INTO network_pending_actions
        (action_id, tenant_id, asset_id, generation, nonce, config_key, config_value,
         status, issued_by, issued_at, version)
        VALUES (:id,:t,:a,:g,:n,'system.ota_upgrade_url',
                'http://127.0.0.1:8090/weaknet-1.1.0-arm64.tar.zst',
                'QUEUED','admin-live-test',:now,1)"""),
        {"id": f"nact-ota-live-{uuid4().hex[:6]}", "t": "tenant-m1-demo",
         "a": "radxa-cubie-a7a", "g": int(max_gen) + 1,
         "n": uuid4().hex, "now": datetime.now(UTC)})
```

下发后板端 `journalctl -u weaknet-server | grep OTA` 可见调度日志；
`data/ota_upgrade.log` 有脚本完整诊断；平台侧动作终态 `APPLIED` 的
`detail` 必须携带真实安装版本号（如 `当前版本: 1.1.0`），版本号对不上
即为异常。

---

## 4. 发布构建流程（Release 流程）

### 4.1 生成/轮换发布密钥对（仅首次或轮换时执行）
```bash
# 生成 ECDSA P-256 密钥对
./tools/gen-release-keys.sh
# 产物：
#   secrets/release_priv.pem (0600，保密，已入 .gitignore，严禁提交到 git)
#   certs/release_pub.pem    (0644，公开分发，已入代码库，部署时自动同步板端)
```

### 4.2 本地一键构建签名固件
```bash
# 1. 指定版本构建（会自动更新根 VERSION，执行 ARM64 原生交叉编译，生成签名与 SHA256SUMS）
./tools/release.sh 1.1.0

# 2. 复用已有 dist-arm64 打包并签名（快速重构）
./tools/release.sh --skip-build

# 产物保存在 artifacts/ 目录下（已入 .gitignore）：
#   artifacts/weaknet-1.1.0-arm64.tar.zst
#   artifacts/weaknet-1.1.0-arm64.tar.zst.sig
#   artifacts/SHA256SUMS
```

### 4.3 GitHub Actions 自动化发布流水线
推送 Git Tag 触发自动构建、签名与发布：
```bash
git tag v1.1.0
git push origin v1.1.0
```
流水线 `.github/workflows/release.yml` 会自动执行交叉编译、使用 GitHub Secret 注入的 `WEAKNET_RELEASE_PRIV_KEY` 签名，并在 Releases 页面挂载所有制品供设备直接升级。

---

## 5. 日常验证与防退化

在开发板运行全面测试套件时，Phase 14 已挂载 OTA 自动化回归测试：
```bash
ssh board 'sudo /home/radxa/weaknet/weaknet-test-full.sh'
```
测试会自动验证：
- `14.1`: `weaknet-cli update status` D-Bus 接口可读性；
- `14.2`: 未签名升级包拦截（验证 fail-closed 门禁）；
- `14.3`: 伪造密钥签名拦截（验证 ECDSA 密钥校验完整性）。

### 5.1 P0-A / P0-B 自测

控制器与恢复器各有独立的单元测试与沙盒演练（都不碰真机、不启服务）：

```bash
# P0-A 控制器单元测试（74 项）：状态机、崩溃矩阵、GC 保守性、投影、
# bootstrap 推断来源、CLI 行为
python3 tools/test_weaknet_ota_ctl.py

# P0-A 沙盒演练（44 项断言，8 节）：bootstrap 接管、正常升级、健康失败回滚、
# 同版本幂等重装、崩溃点判定、GC 跳过、损坏拒绝、首次安装
bash tools/ota_p0a_drill.sh

# P0-B 恢复器单元测试（35 项）：只读事务、健康判据、救援模式、动作编排
python3 tools/test_weaknet_ota_recover.py

# P0-B 沙盒演练（21 项断言，8 节）：无事务、健康转正、尝试耗尽回滚、
# 崩溃循环、控制器缺失兜底、状态损坏救援、救援报告、升级脚本消失仍可恢复
bash tools/ota_p0b_drill.sh
```

演练里的「崩溃点判定」一节用 `--dry-run` 走 `reconcile`：它**只判定不执行**，
所以可以安全地在任意持久事实组合上跑。P0-B 演练用假的 `systemctl` /
`busctl` 替身（放在 `PATH` 最前）驱动恢复器的完整行为链。

### 5.2 部署新版本到开发板

控制器与升级脚本由 `tools/ci.sh` 统一部署（不要手工 scp）：

```bash
./tools/ci.sh              # 编译 + 打包 + 部署 + 真机测试
./tools/ci.sh --local-only # 只编译打包，不碰板子
```

`ci.sh` 的部署顺序是：装控制器 → 装恢复服务 → 建状态目录 → bootstrap 老设备
→ 装升级脚本 → `adopt` 布局迁移与版本切换 → 健康窗口。控制器缺失时升级脚本
**fail-closed 拒绝升级**（不会绕过事务保护裸改指针）；恢复服务缺失只告警，
因为它影响的是"开机自愈"而不是"能不能升级"。

### 5.3 手工查看板端恢复服务状态

```bash
ssh board 'systemctl is-active weaknet-ota-recover.service'
ssh board 'sudo journalctl -u weaknet-ota-recover.service -n 20 --no-pager'
ssh board 'sudo python3 /usr/local/lib/weaknet/weaknet-ota-recover \
    --root /home/radxa/weaknet --state-dir /var/lib/weaknet-ota \
    --ctl /usr/local/lib/weaknet/weaknet-ota-ctl --dry-run'
```

`--dry-run` 只做判定与健康探测，**不执行任何动作**——手工排障时用它。

---
