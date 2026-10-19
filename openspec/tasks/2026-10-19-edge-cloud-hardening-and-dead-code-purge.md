# 边缘+云端杂项清理与加固（O4-O10 / S4 / S6 / S9 残留 / gitleaks / CMake 显式化）

> 日期：2026-10-19
> 分类：工作流外实施（full-code-review 修复批次 3）
> 结果：已完成

## 构思

一次把评审清单里所有"可直接做"的小项收掉，为 O11-O16（云侧真实消费者，
需要另行设计）和板端统一验证腾出干净基线：

- **O4/O8/O9 删孤儿**：`tools/verify_v1_contract.sh`（v1 冻结后零引用）、
  `tools/bt_hci_probe_test.bpf.c`（一次性探针遗留）、根 `init.sh`（旧启动包装，
  三个脚本均可直接调用）——grep 全仓零引用后删除。
- **O7 删零消费者方法**：`TcpConnectMonitor::consumeLostEvents` /
  `getCaptureDiagnostics` 全仓无调用方（同名诊断方法属于 DnsMonitor，
  server.cpp 只取 DNS 的）；连带删除只为其服务的 `impl_->lost_events`。
  `drain_stats` 按评审结论保留（与 DnsMonitor 同构，写侧仍在）。
- **O5/O6/O10 文档纠偏**：README 一键启动路径（脚本在仓库根，不在 LMA 内）、
  legacy 音频补退出条件（bt_hci_traffic 稳定 3 版后独立 change 删除）、
  AI-assisted README 的"整目录弃用"对 2 个活文件不实
  （edge_network_rag.py / network_knowledge_base.py 仍被打包执行）。
- **S4 OPA token 认证**：OPA REST 决策接口此前无认证（backend 网内任意
  容器可直接 POST 求值）。实测 OPA 1.8 语义后落地：
  `--authentication=token` **只解析** Bearer 头放进 `input.identity`，
  真正校验必须配 `--authorization=basic` 求值 `system.authz` 策略——
  用独立 `infra/opa/server_authz.rego`（一个 .rego 只能一个 package，
  不能塞进 policy.rego）做共享密钥比对，密钥经环境变量 `OPA_TOKEN`
  注入两容器。实测：无头 401 / 错 token 401 / 正确 200。
- **S6 默认口令清零**：lite 主栈已是 `:?`，但可选覆盖层还剩
  Grafana `industrial-ops-local`、Marquez 硬编码 `marquez`、Neo4J
  `local-graphrag-password`、Label Studio 演示口令——统一 `:?` +
  dev_lite 生成。
- **S9 残留**：`tools/ci.sh --commit` 的 `git add -A` 在共享 worktree
  下会把其它会话的未跟踪文件扫进归档提交。
- **gitleaks 迁入根 ci.yml**：嵌套工作流 GitHub 不注册（既有结论），
  LMA 里的 secret-scan 从未执行过。
- **CMake GLOB → 显式清单**：`server/CMakeLists.txt:4` 的
  `file(GLOB ... CONFIGURE_DEPENDS)` 对新增/删除 .cpp 静默，与仓库
  其余显式 target 风格不一致。

## 实现方式

**删除（O4/O7/O8/O9）**：`git rm` 三个零引用文件；tcp_connect_monitor
hpp/cpp 删两方法 + `lost_events` 成员/写点，`on_tcp_lost` 只写 drain_stats。

**文档（O5/O6/O10）**：README 启动块改为根目录调用一键脚本、`cd LMA`
只包手动步骤；产品定位"已知遗留"表补 3 版退出条件并在"不做"表交叉引用；
AI-assisted README 头部改为"弃用但含 2 个活文件"并点名文件与打包/执行链
（ci.sh:144-145、edge_ai_scheduler 默认路径、weaknet-cli diagnosis --ai），
文件结构按"活/历史"重排。

**S4（TDD，先测后码）**：
- 用 OPA 1.8.0-static 容器穷举实测（--token 不存在、bearer_token 配置键
  无效、token-only 全 200 + 警告），确定可行模式后写
  `infra/opa/server_authz.rego`（`allow := object.get(input,"identity","")
  == opa.runtime().env.OPA_TOKEN`，object.get 兜底保证 401 而非 500）。
- compose.lite.yaml：opa 服务 `--authentication=token
  --authorization=basic` + 环境注入 + 挂载新 rego + 健康检查改
  `--data /policy` 目录加载；`x-app-environment` 锚点补
  `OPA_TOKEN: ${OPA_TOKEN:?...}`。
- 客户端：`OpaPolicyClient` 增 `token=` kwarg 带 Bearer 头；密钥经
  SecretProvider（`SecretName.OPA_TOKEN`，环境映射 `OPA_TOKEN` 与服务端
  同名）——遵守"Settings 不放凭据"教义，不用 Settings 字段。
  三处构造点（runtime/runner/deployment cli）启动期取值（缺失 fail-fast）。
- Vault 后端若无 opa_token 键会启动即崩：新增 `LazySecretProvider`
  （Vault 主查 + **仅 OPA_TOKEN 豁免**回退环境变量），迁移期不断链。
- 测试 `tests/test_opa_token_auth.py`：Bearer 头有/无、连接错误映射、
  环境变量同名断言、Lazy 豁免名单语义（5 用例）。

**S6**：三个覆盖层 compose 共 8 处默认口令 → `:?`；dev_lite 生成列表补
IOAP_GRAFANA_ADMIN_PASSWORD/MARQUEZ_PASSWORD/IOAP_NEO4J_PASSWORD/
M3_LABEL_STUDIO_TOKEN/M3_LABEL_STUDIO_PASSWORD；README 密钥节补"本地口令
无默认值"与 `/metrics` 暴露面说明（S9）。

**S9 残留**：ci.sh 归档提交改 `git add -u` + 仅放行 `openspec/` 新增，
并加 `git diff --cached --quiet` 守卫（porcelain 非空但全是他人未跟踪
文件时不再空提交报错）。

**gitleaks**：根 `ci.yml` lint-and-check 作业加
`gitleaks/gitleaks-action@v2`（checkout 改 fetch-depth: 0），LMA 嵌套
ci.yml 头注更新迁移清单。

**CMake**：GLOB+4 filter 换59 项显式 `set(SERVER_SOURCES ...)`；
**有意不含** `src/update_status.cpp`——那是并发会话的未跟踪 OTA 文件，
只被 GLOB 顺带编译，显式化后需由其作者自行登记（新增文件必须显式声明
正是本次改动的目的）。

## 结果

- x86 全量 ctest **53/53**；LMA pytest `-m "not live"` **299 passed**
  （含 5 个新 S4 用例）。
- compose.lite/integration/observability YAML 校验通过；缺 OPA_TOKEN 时
  `docker compose config` 按预期 fail-fast；ci.sh `bash -n` 通过。
- S4 端到端语义由 OPA 1.8.0 容器实测背书（401/401/200）。
- 遗留：O11-O16 云侧真实消费者（另行批次）；并发会话 OTA 文件需在
  显式清单下自行登记 `update_status.cpp` 与测试 target；其 stash 内含
  旧版 dbus_service 改动，与 O3-B 的删除存在必然冲突，由该会话自行解决；
  板端统一验证（`BOARD=board ./tools/ci.sh`）待全部批次完成后执行。
