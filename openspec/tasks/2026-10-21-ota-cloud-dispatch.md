# 云端下发 OTA 升级（pending_actions 通道）与回执假成功修复

> 日期：2026-10-21
> 分类：工作流外实施（承接 [[2026-10-20-ota-phase3-remote-and-hardening]]）
> 结果：待填（端到端真机闭环验证完成后回填）

## 构思

OTA Phase 1/2/3 打通了板端本地升级、ECDSA 签名链与远程 URL 拉包，但升级
指令仍只能由登录开发板的人发起。平台侧已有的下行通道
（`POST /edge/telemetry` 响应携带 `pending_actions`，Ed25519 签名 + generation
防重放 + claim_token 回执）只承载配置调参动作。本任务把 OTA 纳入该通道：

- 云端排一条 `config_key=system.ota_upgrade_url` 的动作（value = 固件 URL），
  板端异步执行升级并回传 APPLIED/REJECTED 终态——运维在平台上一键升级现场
  网关，不碰板子。
- 双端白名单契约：`trialableKeyNames()`（C++）与 `CONFIG_KEYS`（Python）逐位
  对齐，12 位金标 `catalog_version` 跨语言测试守护（本次更新为 `703e0ceb4b62`）。

首版实现真机联调时暴露了一个**比功能缺失更危险的缺陷**：云端收到
`APPLIED（当前版本: 1.0.0）`——回执报成功、板子却没升级（下称"假 APPLIED"）。
根因是两层叠加（详见"实现方式"第 2、3 点），本任务一并修复并以端到端真机
闭环作为验收。

## 实现方式

**1. 动作接线（`system.ota_upgrade_url`）**
- `server/src/weaknet_config.cpp`：白名单追加 `system.ota_upgrade_url`（共 42 键）。
- `Large-Model-Application/.../edge_action_catalog.py`：`CONFIG_KEYS` 同步追加，
  `TOP_LEVEL_CATALOG_VERSION` → `703e0ceb4b62`；两端金标测试同步更新。
- `server/src/edge_telemetry_exporter.cpp` `applyPendingActions`：新分支解析该
  key → `scheduleOtaUpgrade()`（校验 http(s) 前缀 → 落盘回执 → fork+setsid 执行
  `weaknet-local-update.sh apply <url>`）；调度成功即推进 generation 并跳过
  统一回执（终态由回执通道异步上报）。
- 重放防护：同一 action 在途（回执文件存续）时静默跳过不重放回执；已在途的
  另一条 OTA 动作拒绝并发调度；回执定稿后把去重表 `executed_action_ids_`
  纠正为真实终态。

**2. 回执两段式生命周期（修复假 APPLIED 的第一层）**
- 调度时只写 `result=IN_PROGRESS`（含 action_id/claim_token/generation/target_url）。
- `SUCCEEDED` **只能**由升级脚本在 `.committed` 落盘后的唯一成功出口显式写入；
  `FAILED` 由脚本 EXIT 陷阱写（不看退出码）。
- 上报器 `checkAndEmitPendingOtaReceipt()`（`start()` + 主循环 ≤5s 双路轮询）
  只认脚本写下的终态；`IN_PROGRESS` 超时 600s 视为升级进程已死按 FAILED 上报。
- 旧实现的错误：回执文件存在即无条件报 APPLIED——升级脚本根本没跑成（下载
  失败/被杀）也会让云端记"升级成功"。单测 4 例钉死新语义
  （`EdgeTelemetryOtaReceipt.*`：在途不发、超时报 REJECTED、SUCCEEDED 带真实
  版本、FAILED 报 REJECTED）。

**3. cgroup 迁出（修复假 APPLIED 的第二层，真机根因）**
- 真机故障链：exporter fork 的升级脚本身处 `weaknet-server.service` 的 cgroup
  （`setsid` 只脱离会话、脱离不了 cgroup）→ 脚本执行到
  `systemctl stop weaknet-server`（KillMode 默认 control-group）→ systemd 把
  整个 cgroup 含脚本一起 SIGTERM → 脚本死在下载/落位之后、翻转之前；旧版
  EXIT 陷阱在 SIGTERM 下读到 `$?`=0（bash 实测行为）写下 SUCCEEDED → 服务
  优雅关停途中上报器把假 APPLIED 发给了云端；服务停后无自动重启
  （Restart=on-failure 对干净 stop 不生效），板子还失联了。
- 修复：
  - 脚本 `escape_service_cgroup()`：发现自己在服务 cgroup 内时先写
    `/sys/fs/cgroup/cgroup.procs` 迁到根 cgroup（cgroup v2），再动 systemctl；
    手动 ssh/sudo 执行时为空操作。
  - 陷阱改为"未抵达成功出口 = FAILED"，不再看退出码（SIGTERM 下 $? 不可靠）。
  - 防砖兜底：`svc_cmd` 维护"服务被我停了"标记，EXIT 陷阱若死在 stop→restart
    窗口内则把服务拉回来。

**3.5 release 目录属主/权限归一化（真机第二层缺陷）**
- 修复 cgroup 迁出后的首次真机升级又暴露新问题：升级成功、云端回执也带了
  真实版本号 1.1.0，但 `weaknet-cli` 在 radxa 下 Permission denied——服务
  进程受 unit `CapabilityBoundingSet`（仅 NET_RAW/NET_ADMIN/BPF/PERFMON/
  DAC_OVERRIDE）限制，无 CAP_CHOWN，tar 的 `--same-owner` 静默失败，全部
  文件落成 root 属主；归档根目录权限又随构建机 umask 漂移成 700。两层
  叠加把普通用户锁在 `releases/<ver>` 目录外。
- 修复：`normalize_release_perms()`（chown 尽力而为 + 目录 755/文件
  `u=rwX,go=rX` 必做）在 apply 落位与 adopt 迁入后立即执行；staging 创建
  改用 `mkdir -m 0755`。沙盒回归钉死"077 umask 归档 → 顶层 755、CLI 可执行"。

**4. 可观测性与工程加固**
- fork 子进程 stdout/stderr 从 `/dev/null` 改为追加写 `data/ota_upgrade.log`
  （本次排查全靠侧写，就是因为脚本输出被丢弃）。
- `tools/ci.sh`：板端测试脚本 scp/chmod 失败不再触发 `set -e` 静默杀死整个
  流程（真机踩过：板端残留 root 属主旧文件 → scp EACCES → Step 4 全没跑）。
- 文档同步：`docs/OTA固件更新与安全发布指南.md` 新增 3.5 云端下发章节
  （链路图、回执生命周期表、三条勿回退约束、平台侧注入示例）。

## 结果

**端到端真机闭环已验证（云端一键升级 → 板端真实升版 → 回执真实版本号）：**

- gen=7（修复前代码，复现缺陷用）：云端记 `APPLIED (当前版本: 1.0.0)`——
  板子实际未升级、服务还被停了。这就是本次要修的"假 APPLIED"，作为反面
  证据留在动作历史里。
- gen=8（修复后代码）：板端 09:07:02 收到动作 → 下载 9.6MB 签名包 → 验签 →
  cgroup 迁出 → stop/flip/重启 → 健康窗口 3 连绿 → 转正；新服务实例 09:08:16
  拾取 `SUCCEEDED` 回执排队 APPLIED，云端 75 秒内闭合：
  `status=APPLIED, result_detail="OTA 升级完成并转正 (当前版本: 1.1.0)"`。
  板端 `readlink current` → `releases/1.1.0`，`weaknet-cli update status`
  报 `installed_version=1.1.0, current_committed=true`。
- gen=9（归一化验证，同版本幂等重装）：走同一条服务子进程路径，升级后
  `releases/1.1.0` 顶层 755、radxa 直接可执行 `weaknet-cli`（无需手工
  修复），云端同样如实回 `当前版本: 1.1.0`。
- 升级诊断全程留痕于板端 `data/ota_upgrade.log`（含 cgroup 迁出与健康窗口
  通过记录）。

**本地/单测证据：**
- x86 全量 53 套件全绿；新增 4 例 `EdgeTelemetryOtaReceipt.*` 单测钉死回执
  语义（在途不发、超时报 REJECTED、SUCCEEDED 带真实版本、FAILED 报
  REJECTED）。
- 沙盒回归：URL 拉包升级成功 → 回执 SUCCEEDED；SIGTERM 中途回执 FAILED
  （旧实现会假 SUCCEEDED，真机根因复现）；077 umask 归档 → 归一化后顶层
  755、CLI other 可执行。
- 板端全量回归（weaknet-test-full.sh）两次部署均 FAIL: 0。

**遗留观察（不阻塞）：**
- `releases/<ver>` 下文件属主在服务子进程路径下为 root（无 CAP_CHOWN 改
  不了），权限已归一化、功能无碍；手动 ssh 路径下会正常归主为 radxa。
- tar 解包时刷 "Cannot change ownership"（服务子进程无 CAP_CHOWN），
  属预期噪声；日志里该告警出现 108 次属正常计数，不是故障。

**收尾时顺带修好的板端环境问题（2026-10-21 收尾阶段）：**
- **板端时钟错位一天多**：热点 DHCP 场景下板子拿不到 DNS，NTP 主机名解析
  不了，系统时间一直停在错误值（tar 报"时间戳在未来"）。修法：改用 IP 直连
  的 NTP 源（`/etc/systemd/timesyncd.conf.d/ntp.conf`：
  `NTP=203.107.6.88 120.25.115.20 202.108.22.5`），现 `NTPSynchronized=yes`。
- **板端 DNS 失效**：DHCP 下发的 resolver 192.168.137.1（热点网关）不响应
  DNS 查询。修法：NetworkManager 固定公网 DNS
  `nmcli con mod tan ipv4.dns "223.5.5.5 119.29.29.29" ipv4.ignore-auto-dns yes`，
  板端外网恢复正常（curl 返回 200）。
- **隧道双开互抢 8000 端口**：板端同时存在 `cloud-tunnel.service`（旧）与
  `weaknet-tunnel.service`（新），两者都 `-L 8000:127.0.0.1:8000`，
  后启动的那个必然因 "Address already in use" 反复重启，云端 2222 反向端口
  随之失效（本会话开头 ssh board 全面连不上的根因）。修法：
  `systemctl disable --now cloud-tunnel.service`，只保留 weaknet-tunnel.service。
  该重复单元是 Phase 3 遗留，建议在板端清理脚本/文档中同步说明。

**最终端到端复验（gen=10，2026-10-21 06:37 板端时钟已修正后）：**
云端下发 → 板端 06:37:54 收到并调度 → 06:39:04 排队 APPLIED（ver=1.1.0）→
云端 `status=APPLIED, result_detail="OTA 升级完成并转正 (当前版本: 1.1.0)"`，
全程 70 秒闭环；板端 `readlink current` → `releases/1.1.0`，
`weaknet-cli update status` → `installed_version=1.1.0, current_committed=true`，
无 pending receipt、无 staging 残留。
