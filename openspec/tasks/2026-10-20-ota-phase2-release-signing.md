# OTA Phase 2 发布签名链（ECDSA P-256 制品签名与强制验签）

> 日期：2026-10-20
> 分类：工作流外实施（用户指示不走 OpenSpec change，承接 [[2026-10-19-ota-local-update]]）
> 结果：已完成（本地构建 + 开发板 5 场景真机验签全通过）

## 构思

Phase 1 在板端验签处留了占位：无公钥告警放行、有公钥 fail-closed。Phase 2 的目标是把这条占位转正为**生产级强制验签闭环**：发布侧能一键产出"包 + 签名 + 校验清单"的可追溯制品，板端固化公钥后拒绝一切未签名/伪造签名/被篡改的升级包。

**选型关键转折（实测驱动）**：原计划 Ed25519。真机实测发现板端 Debian Bullseye 自带 OpenSSL CLI 为 1.1.1w，`pkeyutl`/`dgst` 均不支持 Ed25519 的命令行签名与验签（`Key type not supported`；OpenSSL 3.0 起才支持 CLI）。选项：(a) 升级板端 OpenSSL 风险大（系统库依赖面广）；(b) C API（`EVP_PKEY`）需引入专用小工具维护成本高；(c) **改用 ECDSA NIST P-256 (prime256v1) + SHA256**——两代 OpenSSL CLI 原生可用，安全强度等同 128 位对称加密，实测 `openssl dgst -sha256 -verify` 返回 Verified OK。选 (c)。板端上行遥测的 Ed25519 验签走 C API `EVP_DigestVerify`，与本链互不影响，维持原样。

## 实现方式

**新增**
- `tools/gen-release-keys.sh`：生成 ECDSA P-256 密钥对；私钥 → `secrets/release_priv.pem`（0600，已被 `.gitignore` 的 `secrets/` 覆盖），公钥 → `certs/release_pub.pem`（0644，入库分发）。`--force` 才允许重新生成（重新生成会使旧包验签失效）。
- `tools/release.sh`：一键发布构建。版本校验（可带新版本号参数更新 VERSION）→ ARM64 裸机原生交叉编译 → `weaknet-local-update.sh package` 打包+签名（`WEAKNET_RELEASE_PRIV`）→ 本地公钥自验 → `artifacts/SHA256SUMS`。产物：`artifacts/weaknet-<ver>-arm64.tar.zst{,.sig}`（`artifacts/` 已入 `.gitignore`）。支持 `--skip-build` 复用现有 dist-arm64。
- `.gitignore` 补 `artifacts/`。

**修改**
- `tools/weaknet-local-update.sh`：
  - `verify_signature` 转为**默认 fail-closed**：无公钥直接拒绝（不再告警放行），验签命令改 `openssl dgst -sha256 -verify`；`verify/apply` 新增 `--insecure`（= `--allow-unsigned`）显式逃生口。
  - `cmd_package` 签名命令改 `openssl dgst -sha256 -sign -out`。
- `tools/ci.sh`：部署时把 `certs/release_pub.pem` 同步到板端 `config/release_pub.pem`。
- `docs/weaknet_cli_usage.md`：3.10 节签名段更新为强制验签语义与选型说明（Ed25519 → ECDSA P-256 及原因）。
- 修复 `cmd_package` 用 `pkeyutl -sigfile` 的参数错误（该参数是 verify 专用，sign 应为 `-out`；x86 自测时发现）。

**验证**
- x86：`release.sh --skip-build` 构建 9.2M 包 + 签名 + `Verified OK` 自验 + SHA256SUMS。
- 开发板真机 5 场景（公钥与新版脚本先行同步至板端）：
  1. 未签名包 → 拒绝（缺 `.sig`，fail-closed）；
  2. 攻击者私钥伪造签名 → 拒绝（密钥不匹配）；
  3. 真包篡改 1 字节（签名原样保留）→ 拒绝（签名不匹配）；
  4. 正版签名包 `verify` → 通过（`ECDSA (P-256) 数字签名校验通过`）；
  5. 正版签名包 `apply` 全链路 → 验签通过 → 健康窗口 3 连绿 → 转正 1.0.0，服务 active。

**过程插曲**：板↔云反向隧道多次挂死（2222 端口被卡死连接反复抢占、2223 尝试后板端重建于 2222），最终经用户清理端口并重建隧道恢复；SSH 配置恢复为 `127.0.0.1:2222`。

## 结果

Phase 2 签名链交付完毕：`gen-release-keys.sh`（密钥）→ `release.sh`（构建+签名+校验清单）→ `ci.sh`（公钥分发）→ 板端强制验签（fail-closed）四段全通，真机 5 场景全部符合预期。

遗留与后续：
- 私钥尚未接入 CI Secret（当前为本地 `secrets/release_priv.pem`）；GitHub Actions 自动发布流属 Phase 2 的 CI 延伸，另行实施。
- Phase 3（云端审批 + `pending_actions` 远程下发触发升级）未启动。
- 远端传输时板↔云隧道稳定性欠佳（缺保活配置），后续排障注意。
