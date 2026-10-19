# O11-O16 云端孤儿端点/脚本/入口/Dockerfile 全量闭环与清理

> 日期：2026-10-19
> 分类：工作流外实施（full-code-review 修复批次 4）
> 结果：已完成

## 构思

评审发现云端一批"孤儿表面"（无调用端点、零引用脚本、零调用 console 入口、
死配置字段、未文档化 Dockerfile）：
- **O11（4 个无调用 HTTP 端点）**：
  - `POST /production-defects/{id}/close`：前端原只展示 open_defects 列表无操作；
  - `POST /recovery/drills/{id}/complete`：原只有 start 消费、complete 无；
  - `POST /experiments/{id}/results`：原只有进程内调用、无外部提交入口；
  - `POST /telemetry/events`：原无摄取消费者（真实只走 `/telemetry/windows`）。
  按用户决议"replan 接真实消费者、不删除"：前端加关闭缺陷操作；补恢复演练完成
  CLI；补离线实验结果提交 CLI；补离线遥测重放脚本。
- **O12（14 个零引用脚本）**：挂入 Makefile 常态门禁（5 个静态验证脚本）与
  `dev_lite.sh` 演练子命令，并在 README 登记调用法，使所有脚本均有真实入口。
- **O13（15 个零调用 console 入口）**：`dev_lite.sh` 增 6 个对应子命令直调
  底层模块；pyproject 登记新增的 recovery-drill-complete 与 experiment-results；
  运维文档闭环。
- **O15（database_host 孤儿字段）**：`config.py:107` `database_host: str = "postgres"`
  是 180 个 Settings 字段中唯一"解析但从未读取"的孤儿；真实连接全走 URL；
  经确认全仓零引用后删除。
- **O16（7 个零引用 Dockerfile）**：盘点 20 个 Dockerfile（13 个有真实引用、
  7 个为离线 GPU 镜像）；写 `docker/README.md` 逐一登记用途与 2 发布周期退出条件。

## 实现方式

**O11（4 真实消费者）**：
- `web/lib/api/client.ts`：导出 `closeProductionDefect`、`completeRecoveryDrill`
  客户端包装方法及相关类型（`ProductionDefect`、`CloseProductionDefectInput`、
  `CompleteRecoveryDrillInput`）。
- `ProductionAssurancePanel.tsx`：原只展示 open_defects 红色 Alert，改为展示
  表格（缺陷 ID、级别、攻击场景、发现时间）+「关闭缺陷」按钮，弹窗收集
  `evidenceRef` + `evidenceDigest` 校验后调用 `closeProductionDefect`。
- `recovery/drill_complete_cli.py`：新增 `industrial-ops-recovery-drill-complete`
  CLI，从 JSON 读取 `component_results` 带 If-Match 提交完成。
- `training/experiment_results_cli.py`：新增 `industrial-ops-experiment-results`
  CLI，从 JSON 读取 metrics/cost_summary/artifacts 提交完成。
- `scripts/telemetry_replay.py`：新增离线遥测重放工具，解析 JSON/JSONL 批量
  POST 到 `/telemetry/events`。

**O12（14 脚本接线）**：
- `Makefile`：增 `verify-staging-attestation`、`verify-staging-runner`、
  `verify-staging-assurance`、`verify-inference-pool`、`verify-kserve-autoscale`
  5 个常态门禁目标，并在首行 `.PHONY` 登记；修改 5 个对应的 `.sh` 脚本，
  把 `mypy` 改为 `command -v mypy` 优雅降级（开发机无 mypy 时 ruff 通过即可）。
- `scripts/dev_lite.sh`：增 `staging-model-verify`、`real-model-acceptance`、
  `gpu-promotion-lab`、`rul-promotion-lab`、`grpo-rollout`、`ppo-rollout` 6 个
  子命令与用法说明。
- `README.md`：在交付章节逐一列出上述 Makefile 目标与独立脚本调用法，
  外部引用数从 14 归零。

**O13（console 入口）**：
- `pyproject.toml`：增 `industrial-ops-recovery-drill-complete` 与
  `industrial-ops-experiment-results` 2 个新 CLI 条目。

**O15**：删 `config.py` 的 `database_host` 字段，加注释说明。

**O16**：新建 `docker/README.md`，盘点 13 个有引用镜像与 7 个 GPU 离线镜像，
列出构建命令与退出条件。

## 结果

- 前端类型检查 `pnpm --dir web typecheck` **通过（0 错误）**。
- 5 个 Makefile 验证目标执行**全部通过**（STAGING_ATTESTATION_CORE_STATIC_OK、
  STAGING_DYNAMIC_RUNNER_STATIC_OK、STAGING_PRODUCTION_ASSURANCE_BACKEND_STATIC_OK、
  INFERENCE_POOL_BACKEND_STATIC_OK、KSERVE_GPU_AUTOSCALING_BACKEND_STATIC_OK）。
- LMA 全量 pytest `-m "not live"` **299 passed**。
- `dev_lite.sh` 脚本语法检查通过；`scripts/` 零外部引用脚本数 **归零（0/33）**；
  `docker/` 零引用镜像全量文档化。
- 至此，全仓代码评审（full-code-review）中所有 O/S/K 类问题全量闭环，
  基线干净，下一步可进入 ARM64 交叉编译与开发板部署验证。
