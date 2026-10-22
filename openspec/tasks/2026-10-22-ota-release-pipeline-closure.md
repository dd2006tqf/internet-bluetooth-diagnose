# OTA 发布流水线实跑闭环与五项收尾

> 日期：2026-10-22
> 分类：工作流外实施（承接 [[2026-10-21-ota-cloud-dispatch]]）
> 结果：已完成（GitHub Release 实跑 + 板端真实升级闭环）

## 构思

OTA Phase 1/2/3 的代码与云端下发链路已在 [[2026-10-21-ota-cloud-dispatch]]
闭环，但有五项收尾事项横在"可发布"之前，其中最重的一项是：
**`.github/workflows/release.yml` 从未实跑过**——仓库没有 tag、没有任何
Actions secret，且 `release.yml` 只存在于 `dev/feature`（`main` 落后 219
个提交），GitHub 从未注册这个 workflow。

选择在这个时间点做，是因为"发布流水线没跑过"等于"发布能力不存在"：
板上能装的固件目前只能由开发者本机 `tools/release.sh` 手工产出，签私钥
在开发机上，制品不进任何可追溯的发布渠道。云端下发 OTA 的 URL 指向
GitHub Release，没有 Release 就没有云端下发的落点。

不做进 OpenSpec change 的理由：这是对既有 CI 基础设施的修复与验证，不
改变产品行为契约；且过程中发现的问题全部属于"构建/发布等工程通路"，
按 [[docs/ai/implementation-economy.md]] 的判据不产生产品 surface。

## 实现方式

### 1. 首次实跑暴露的 6 个真缺陷（都不是"配置没填"级别的）

release.yml 从未跑过，所以每一个缺陷都是首次暴露。按被发现顺序：

**① 容器权限（`docker exec` 继承 `--user`）**
容器以 `--user "$(id -u):$(id -g)"` 启动（为了让 `/src` 产物归 runner
用户），但 `docker exec` **继承**容器的 uid。于是：
- `apt-get install` → `Could not open lock file /var/lib/dpkg/lock-frontend`
- sysroot 落 `/opt` → `mkdir: Permission denied`
修法：需要写系统路径的步骤一律 `docker exec --user root`，编译与打包
仍以 uid 1000 跑。

**② x86 runner 上 `--platform linux/arm64` 容器启动即退出**
`docker/setup-buildx-action` 只给 buildx 内部注册 QEMU，**不给宿主
docker 守护进程注册 binfmt**，于是容器一启动就退出，下一步报
`container ... is not running`。
修法：`runs-on: ubuntu-24.04-arm`（原生 aarch64，公开仓库免费），去掉
`--platform`，并加"确认 runner 架构"前置断言。容器**保留**，但作用变了：
不再是"模拟 ARM64"，而是锁定 glibc 2.31（bullseye）基线——runner 是
Ubuntu 24.04（glibc 2.39），直编的二进制拿到板端跑不起来。

**③ 假绿灯：`docker exec bash -c '多行脚本'` 吞掉失败**
退出码只取**最后一条命令**。apt 失败 → cmake 找不到编译器 → 6 个
`install` 全部 `cannot stat` → 但最后一条 install 成功 → 整步退出 0
→ 工作流报 success，并发布了一个 **26 KB** 的残缺包（正常 3.5 MB）。
修法：所有 `docker exec` 内多行脚本补 `set -euo pipefail`。

**④ bullseye 已归档**
Debian 11 结束支持后整体移入 `archive.debian.org`，`deb.debian.org`
上的 bullseye-security 池已清空——实测整套安全更新包（libc6-dev /
perl / git / libcurl4 / openssl / libcap2 …）**全部 404**，apt 报
`Unable to fetch some archives`，连带 `git`/`make` 都装不上。
修法：源换 `archive.debian.org`（bullseye main + bullseye-security）+
`-o Acquire::Check-Valid-Until=false`（归档的 Release 已过期）。

**⑤ `board-assets/vmlinux.h` 被 `.gitignore` 误伤（仓库级长期缺陷）**
`.gitignore` 的裸 `vmlinux.h` 按任意层级匹配，把 `board-assets/` 下的
**预置构建输入**一并忽略了——那份不是产物。于是 `git clone` 拿不到它，
`generate_vmlinux.sh` 落到 "not found" 分支写占位文件，eBPF 编译随即
报满屏 `unknown type name '__u64'`。
本地一直能编过是因为开发机上有这份文件，**CI 每次全新 clone 必然失败**。
这个缺陷一直存在，只是此前从没有任何 CI job 真正编译过 eBPF
（`build-and-deploy.yml` 从未被触发）。
修法：`vmlinux.h` / `server/vmlinux.h` 改为前导 `/` 锚定层级，只忽略
bpftool 生成到根与 `server/` 下的副本；`board-assets/vmlinux.h` 入库
（2.4 MB / 116589 行，取自开发板 BTF）。

**⑥ 缺 Secret 时用临时私钥顶替**
临时密钥签出的包板端**必然**验签失败（板端 `config/release_pub.pem` 是
仓库 `certs/release_pub.pem` 的分发副本），发布一个必然被 REJECTED 的
固件比不发布更糟。
修法：fail-closed，缺 Secret 直接 `exit 1`。

另外补了**产物健全性断言**：仅靠 `set -e` 只能保证命令没报错，所以再按
内容核对——服务端二进制必须是 AArch64 ELF、`NEEDED` 必含 `libssl.so.1.1`
与 `libcurl.so.4`（sysroot 没装上时 CMake 是**静默降级**的，产物照样
链接成功但缺 HTTPS 探测与上行遥测）、eBPF 程序数 ≥ 12。

### 2. 私有 sysroot 脚本（`setup_openssl_sysroot.sh` / `setup_libcurl_sysroot.sh`）

- **下载会无限期挂死**：实测 perl 路线连上 snapshot.debian.org 后长时间
  无响应（不是立刻 509），无超时的下载把整个 sysroot 步骤挂死，永远走
  不到下一个源。补 socket 15s + `SIGALRM` 90s 兜底 + curl/wget 超时。
- **单源假设**：改为 snapshot → tuna → aliyun → ustc 依次回退
  （`SNAPSHOT_BASE` 显式指定时只用它）。四个固定 `.deb` 在三个镜像站
  的 SHA256 与硬编码常量逐一实测相符。
- libcurl 运行时包（`libcurl4_7.74.0-1.3+deb11u13`）此前从仓库取用但
  **不校验**，补写死 `RUNTIME_DEB_SHA256`。

### 3. 其余四项

- **② 制品属主 root**：`--no-same-owner` 解包 + `--owner=0 --group=0`
  打包 + `normalize_release_perms()`，已在 `ee00da4` 实现，本次部署到板
  端后实测 `releases/1.1.0` 为 `radxa:radxa 755`。
- **③ `update verify` 选项不全**：`--allow-downgrade` / `--insecure` 已
  实现并随本版上板。
- **④ 隧道双单元**：板端 `cloud-tunnel.service` 已物理删除，只剩
  `weaknet-tunnel.service`（active + enabled），`ci.sh` 里加了清历史
  单元的逻辑。
- **⑤ `ota_upgrade.log` 无轮转**：服务端 256 KiB 阈值 → `.1`，已随本版
  上板（此前板端跑的二进制是 10-20 的，轮转代码根本没到板子）。

## 结果

### GitHub Actions 实跑（v1.1.0 tag）

四次触发，逐次收敛（每次都是真实缺陷，不是环境抖动）：

| 次数 | 提交 | 结果 | 暴露的问题 |
|---|---|---|---|
| 1 | c7c83d9 | failure | 容器启动即退出（x86 runner 无 binfmt） |
| 2 | 3933da0 | **success（假）** | 26 KB 残包：apt 404 + 吞退出码 |
| 3 | f094002 | failure | vmlinux.h 缺失 → eBPF 编译失败（`set -e` 生效，不再假成功） |
| 4 | 27f2f1b | success（真实） | — |

Release 制品（`v1.1.0`，非 draft）：
- `weaknet-1.1.0-arm64.tar.zst` — 5,023,773 字节
- `weaknet-1.1.0-arm64.tar.zst.sig` — 72 字节
- `SHA256SUMS` — 192 字节

### 独立复核（不只信 CI 的 success）

- 下载回归制品，用 `certs/release_pub.pem` 验签 → `Verified OK`
- 包内 27 个文件、12 个 eBPF 程序；服务端为 AArch64 ELF，
  `NEEDED` 含 `libssl.so.1.1` / `libcrypto.so.1.1` / `libcurl.so.4`
- 包内 `server/bin/weaknet-dbus-server` 的 SHA256 = `093fa0b9…`，
  与板端当时运行的二进制**逐位相同**——发布流水线可复现板端固件
- 板端下载该制品，SHA256 与 `SHA256SUMS` 逐位一致

### 板端真实升级（生产路径）

`weaknet-cli update apply` 走完整链路：读当前版本 → **fail-closed 验签**
（第一次故意不带 `.sig`，被拒：「已配置发布公钥但缺少签名文件」）→
`manifest.sha256` 全量校验（26 个文件）→ 原子翻转 → 健康窗口 3 连绿 →
转正。结果：`installed_version=1.1.0, current_committed=true`，
`releases/1.1.0` 属主 `radxa:radxa 755`，服务 active，无 staging 残留。

同版本幂等重装（包内容与板端一致），因此是**发布通路**的验证，不是
新功能的验证。

### CI 回归

`dev/feature` 上四个 job 全绿（`静态规范检查` / `x86 编译与全量单元测试` /
`OpenAPI 契约与前端构建门禁` / `云端 Python 测试与连接路径静态检查`）。
一次 `OpenAPI` job 因 npm registry 返回 503 失败，重跑即绿——上游瞬时
故障，与本次改动无关。

### 板端网络事实（本次实测，对 OTA 运维有直接影响）

- 板端到 GitHub Release 的下载速率约 **107 KB/s**（5 MB 包 47 秒）。
  这个速度下 `curl` 默认无超时会长时间卡住；现有升级脚本的
  `--speed-limit 1024 --speed-time 60` 阈值（1 KB/s × 60s）对此有余量，
  但**偶发整段连接超时**（实测两次 rc=28），此时升级会干净失败、不触碰
  任何文件（已验证：失败后 current 未变、无 staging 残留、服务 active）。
- 反向隧道（`weaknet-tunnel.service`）在长耗时命令期间会断，恢复后
  `ssh board` 立即可用。OTA 本身由板端 `setsid` 子进程执行，不受隧道
  中断影响——这是 cgroup 迁出 + setsid 设计的实际收益。

### 遗留（不阻塞）

- **密钥已轮转过一次**：2026-10-21 容器测试误覆盖 `secrets/release_priv.pem`，
  已从会话记录重建并校验（`KEYPAIR_OK`，指纹 `f8cd5849…` 与已提交公钥
  一致）。但重建意味着私钥材料出现在本机会话 JSONL 里。仓库是公开的，
  私钥进 Actions Secret 后任何有写权限者都能签出板端接受的固件。
  **建议正式对外发布前轮转一次密钥对**。
- `release.yml` 仍在 `dev/feature`，`main` 落后 219 个提交；按用户指示
  本次不合并至 main。tag `v1.1.0` 打在 `dev/feature` 的 `27f2f1b` 上。
- GitHub Actions 里 `WEAKNET_RELEASE_PRIV_KEY` 已配置（2026-10-22T02:18:01Z）。
