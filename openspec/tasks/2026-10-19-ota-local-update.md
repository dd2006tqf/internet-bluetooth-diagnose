# OTA Phase 1 本地升级（板端闭环）

> 日期：2026-10-19
> 分类：工作流外实施（用户明确指示不走 OpenSpec change）
> 结果：已完成（本地全量验证 + 沙盒演练通过；真机演练待用户批准部署后补记）

## 构思

当前板端升级靠 `tools/ci.sh` 从开发机 rsync `--delete` 覆盖：无版本概念、无原子性、无回滚，两块板以上即不可运维。仓库已有可复用地基（`ConfigTransaction` 180s 试用看门狗、`EVP_DigestVerify` 验签模式、`HealthCheck`/`GetEbpfMonitorHealth` 健康判据、ActionRegistry 目录指纹契约），但 OTA 本身零实现。

Phase 1 目标：**板端本地闭环**——解包校验 → 版本单调检查 → 原子切换 → 重启 → 健康窗口 → 转正/自动回滚，零云端依赖。这是后续云侧制品链（Phase 2，Ed25519 发布密钥 + 制品下发）与审批执行链（Phase 3）的前置地基。

不走 OpenSpec change 的原因：用户明确选择工作流外实施；改动面为新增文件为主（VERSION、update_status 模块、升级脚本、CLI 子命令）+ 部署路径适配（service/ci.sh），属运维地基建设而非产品行为规格变更。按铁律在本目录留记录。

## 实现方式

**版本与布局**
- 新增根 `VERSION`（一行 semver `1.0.0`），CMake 读入 `project(weaknet VERSION ...)`。
- 板端布局：`releases/<version>/{server,client,lib,...,VERSION,manifest.sha256,.committed}`，`current -> releases/<version>` 为原子翻转点（`ln -sfn` + `mv -Tf`），`server`/`client`/`lib` 等兼容软链 → `current/*` 保持历史路径有效。
- `data/` 下 5 个状态文件与 `/etc/weaknet/config.yaml` 升级全程只读。

**C++ 侧（server）**
- 新增 `server/{include,src}/update_status.{hpp,cpp}`：IsValidVersion / CompareVersions / IsUpgradeAllowed（`dpkg --compare-versions` 语义，拒绝降级）/ ParseManifestLine（拒绝对路径与 `..` 穿越）/ ReadCurrentVersion（readlink）/ VerifyReleaseIntegrity（OpenSSL EVP 流式 sha256，无 TLS 构建降级 NO_CAPABILITY）/ StatusJson。
- `GetUpdateStatus` 只读 D-Bus 方法（无需 root），上报 installed_version / layout / pending / committed。
- `server/test/unit/test_update_status_gtest.cpp`：4 纯逻辑 + 7 文件 I/O 用例（mkdtemp 隔离，fixture sha 由本机 sha256sum 钉死）。

**升级执行体（shell）**
- 新增 `tools/weaknet-local-update.sh`：`status/verify/apply/rollback/adopt/package` 六子命令。apply：解包（tar.zst/gz/tar）→ Ed25519 验签（占位：无公钥告警放行，有公钥 fail-closed）→ 版本策略 → 翻转 + 重启 → 健康窗口（服务 active + HealthCheck + GetEbpfMonitorHealth + **GetUpdateStatus 版本一致**防假绿灯，连续 3 次）→ `.committed` 转正；失败自动回滚 `.previous`（同版本旧副本保留到转正才删，flip 失败先恢复）。
- 诊断一律走 stderr，stdout 只留可捕获数据；`sha256sum -c` 输出显式丢弃（实测踩过 FAILED 行污染命令替换）。

**CLI 薄封装**
- `weaknet-cli update status` 走 D-Bus；`apply/rollback/verify` fork+exec 升级脚本（不经 D-Bus——服务端不能调用中途重启自己），apply/rollback 要求 root。

**部署链切换**
- `tools/weaknet-server.service`：ExecStart/WorkingDirectory 经 `current` 寻址。
- `tools/ci.sh`：产物追加 VERSION + 升级脚本；部署改走 `.deploy-staging` + `adopt releases/${VERSION}`（不再 rsync 直接覆盖 server/，restart 由 adopt 翻转后执行）；停服清理收窄，避免经兼容软链误删 `.release` 的 `.bpf.o`。
- 文档同步：`docs/weaknet_cli_usage.md` 新增 3.10 `update` 子命令章节 + 语法概览 + 版本历史。

**验证手段**
- x86 全量：`cmake --build build-x86 && ctest --test-dir build-x86/server` → **53/53 通过**（含新增 test_update_status_gtest 11 用例，无回归）。
- `bash -n` 两个脚本通过。
- 本地沙盒演练（`/tmp/wn-ota-drill`，`WEAKNET_UPDATE_DRYRUN=1` 跳过 systemctl/busctl）**10 场景全通过**：package 打包、verify 校验、首次 apply、status 查询、篡改包拒绝、同版本幂等重装、正常升级、降级拒绝、`--allow-downgrade` 放行、rollback、adopt 旧布局迁移 + data 隔离。

## 结果

Phase 1 全闭环完成，本地与开发板真机演练全部通过。

**真机演练实测结论（2026-10-19，Radxa Cubie A7A，Bullseye ARM64）**：
1. **首次部署与布局迁移**：`BOARD=board ./tools/ci.sh` 编译（40s 原生交叉）+ rsync 暂存 + adopt 触发，旧扁平布局自动归档为 `releases/legacy-flat`；原子建立 `releases/1.0.0`、`current -> releases/1.0.0` 及兼容软链（`server`、`client`、`lib` 等）；健康窗口通过并写入 `.committed`；冒烟测试（basic、network、ping）全绿。
2. **场景 1（打包与幂等重装）**：`package` 生成 9.3M tar.zst，`.committed` 标记未被包内夹带；同版本 1.0.0 apply 覆盖成功，旧副本暂存并在新版转正后清理。
3. **场景 2（篡改防御）**：解包翻转 1 字节后重新打包（manifest 保持原样），apply 被拒（`manifest 完整性校验失败`），current 链接未被改动。
4. **场景 3（真实版本升级）**：构造 1.1.0 包 apply，原子翻转至 1.1.0，健康窗口通过，`.previous` 正确指向 1.0.0，D-Bus `GetUpdateStatus` 返回 `installed_version: 1.1.0`。
5. **场景 4（降级防御）**：尝试从 1.1.0 apply 1.0.0，版本策略拒绝（`需 --allow-downgrade`），current 保持 1.1.0。
6. **场景 5（手动回滚）**：`rollback` 读取 `.previous` 自动翻回 1.0.0，重启通过健康窗口并转正。
7. **场景 6（坏版本自动回滚）**：构造 1.2.0 包（二进制设为 `exit 1` 脚本，自洽 manifest），apply 重启后健康窗口超时，触发自动回滚，板端自动恢复到 1.0.0，服务恢复 `active`。
8. **状态与动作回归**：`data/` 下 5 个状态文件（history.db、network-epoch、wireless-uplink-state、soak 历史日志）在整个演练中持续读写且无破坏；`CHECK_RESOLVER_CONFIG` 等动作执行正常，无目录漂移。

遗留与后续：
- Ed25519 发布密钥 Phase 2 接入（当前无公钥时告警放行，非强制）。
- 云侧制品链（Phase 2）与审批执行链（Phase 3）另立项。

