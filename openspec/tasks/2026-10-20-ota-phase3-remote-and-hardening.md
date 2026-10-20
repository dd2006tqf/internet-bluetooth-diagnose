# OTA Phase 3 远程闭环与工程加固（拉包器、CI 发布流、GC、回归及隧道自愈）

> 日期：2026-10-20
> 分类：工作流外实施（承接 [[2026-10-20-ota-phase2-release-signing]]）
> 结果：已完成（板端真机 26 项全面回归全绿，PASS: 26, FAIL: 0, SKIP: 1）

## 构思

在完成 Phase 1（本地翻转）与 Phase 2（ECDSA 签名链）后，系统仍存在运维与工程断点：
1. **主线断点**：升级包只能由开发机主动推送，板端缺乏从远程 HTTP/HTTPS URL 自动拉取固件与签名的拉包器；CI/CD 没有自动打包发布工作流；
2. **加固断点**：Phase 2 签名拦截只经过手工验证，未纳入日常回归流水线；`releases/` 目录历史版本无限制膨胀，中途异常退出的 `.staging.*` 残留无清扫；
3. **运维断点**：开发板跨 NAT 隧道无心跳机制，静默断连频繁导致卡死。

本任务一次性将上述一、二、三类断点全面闭环并固化。

## 实现方式

**1. OTA Phase 3 板端拉包器（主线）**
- 修改 `tools/weaknet-local-update.sh`：`cmd_apply` 支持识别 `http://` / `https://` 开头的升级包路径。
- 自动使用 `curl -fSL` 下载远程固件包至 `/tmp`，并同步下载伴随的 `${pkg}.sig` 签名文件。
- 下载完成后自动无缝进入本地 `stage_package` 严格签名校验与原子切换流程，并在完成后自动清理临时下载文件。

**2. GitHub Actions 自动发布流（主线）**
- 新增 `.github/workflows/release.yml`：
  - 触发条件：git tag `v*.*.*` 推送，或 `workflow_dispatch` 手动触发。
  - 流水线：ARM64 容器化构建产物 → 读取 `WEAKNET_RELEASE_PRIV_KEY` Secret 签名 → 调用 `tools/release.sh` 生成固件包、签名及 `SHA256SUMS` → 自动发布到 GitHub Release 并输出板端一键升级命令。

**3. 签名与升级自动化回归（加固）**
- 修改 `tools/weaknet-test-full.sh`：新增 **Phase 14: OTA 升级与签名回归测试**。
  - 14.1: `weaknet-cli update status` D-Bus 接口只读查询与版本解析断言；
  - 14.2: 未签名升级包拦截断言（验证生产态 fail-closed 门禁）；
  - 14.3: 攻击者伪造私钥签名拦截断言（验证 ECDSA P-256 密钥匹配性）。

**4. 空间防膨胀与残留清扫 GC（加固）**
- 修改 `tools/weaknet-local-update.sh`：新增 `cleanup_staging_and_gc` 函数，在每次 `apply` 转正后自动执行：
  - 清扫异常中断残留的 `releases/.staging.*` 临时目录；
  - 历史版本垃圾回收：默认保留最新 3 个版本（当前版本、`.previous` 版本及最近稳定版），过旧版本自动清理释放闪存空间。

**5. 开发板隧道自愈保活服务（运维）**
- 开发板端部署 `weaknet-tunnel.service` systemd 常驻服务（`User=radxa`, `Restart=always`, `RestartSec=3`）；
- 内置 `ServerAliveInterval=10`、`ServerAliveCountMax=3`、`TCPKeepAlive=yes` 心跳探测，彻底根治跨公网隧道挂死难题。

## 结果

真机全面测试验证：
- 运行 `weaknet-test-full.sh`：**PASS: 26, FAIL: 0, SKIP: 1**，Phase 14 OTA 三项回归全绿，服务端日志健康。
- 板端拉包器与 GC 逻辑就位；
- 隧道开机自启并受 systemd 守护。
