# WeakNet OTA 固件更新与安全发布指南

> **版本**：v1.2 (涵盖 Phase 1 本地闭环 + Phase 2 ECDSA 签名链 + Phase 3 远程拉包与空间 GC)  
> **适用平台**：Radxa Cubie A7A (ARM64) / x86_64 宿主机交叉编译环境  
> **核心特性**：A/B 软链接原子翻转、ECDSA NIST P-256 强制验签、180s 健康看门狗、故障自动回滚、状态与配置隔离保护、过旧版本自动 GC

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
```

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
   - 默认保留最新 3 个版本（当前、上一版、次新版），其余过旧版本自动删除，杜绝长期运行挤爆 Flash。

---

## 3. 运维操作手册（Agent 指南）

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
