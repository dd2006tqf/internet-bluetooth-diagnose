# docker/ 镜像构建清单（O16 盘点）

> 2026-10-19 full-code-review O16：`docker/` 下 20 个 `*.Dockerfile` 中
> **13 个有真实引用**（compose 覆盖层 / lab CLI / 供应链脚本），**7 个零引用**
> ——不在任何 compose、CI、脚本或 runbook 中出现。按处置结论"文档化保留"，
> 本文件逐一登记引用关系与退出条件；新增 Dockerfile 必须在此登记并挂上
> 真实构建入口，否则视为孤儿。

## 有真实引用（13）

| Dockerfile | 构建/引用入口 |
|---|---|
| `api.Dockerfile` | `compose.lite.yaml` |
| `web.Dockerfile` | `compose.lite.yaml` |
| `worker.Dockerfile` | `compose.lite.yaml` / `compose.integration.yaml` / `compose.ai.yaml` |
| `event-worker.Dockerfile` | `compose.integration.yaml` |
| `data-worker.Dockerfile` | `compose.integration.yaml` |
| `training-worker.Dockerfile` | `compose.integration.yaml`（README 亦有说明） |
| `quantization-worker.Dockerfile` | `compose.integration.yaml`（README 亦有说明） |
| `ai-worker.Dockerfile` | `compose.ai.yaml` |
| `media-server.Dockerfile` | `compose.realtime.yaml` |
| `model-runtime.Dockerfile` | `scripts/create_supply_chain_statement.py`、`supply-chain.yml`（休眠，见其头注） |
| `gpu-lab-runtime.Dockerfile` | `training/gpu_promotion_lab_cli.py`（GPU 推广实验室） |
| `llama-cpp-runtime.Dockerfile` | `training/gpu_promotion_lab_cli.py` |
| `reranker-calibrated-runtime.Dockerfile` | `simulation/reranker_kserve_readiness.py` |

## 零引用（7）——文档化保留，附退出条件

这些是 **GPU/模型服务运行时镜像**（pytorch CUDA 基底或
`${IOAP_BASE_IMAGE}` 参数化基底），服务于离线 GPU 机器上的 KServe/模型
部署路径：不进 lite/默认 compose（默认部署无 GPU），历史上由人工在 GPU
机上 `docker build -f docker/<x>.Dockerfile` 构建后推私有仓库。

| Dockerfile | 用途 | 退出条件 |
|---|---|---|
| `embedding-runtime.Dockerfile` | embedding 模型服务镜像 | 接入某个真实构建入口（脚本/Makefile/lab CLI 子命令）或连续两个发布周期无人构建即删 |
| `reranker-runtime.Dockerfile` | reranker 模型服务镜像 | 同上（校准版已有真实引用，见上表） |
| `timeseries-runtime.Dockerfile` | 时序（预测性维护）模型服务镜像 | 同上 |
| `rul-runtime.Dockerfile` | RUL 模型服务镜像 | 同上 |
| `rul-local-kserve.Dockerfile` | 本地 KServe RUL 部署镜像 | 同上 |
| `model-router.Dockerfile` | 模型路由（slim 基底） | 同上 |
| `enterprise-candidate-runtime.Dockerfile` | 企业候选模型运行时 | 同上（参数化基底与 `IOAP_BASE_IMAGE` 一致） |

构建示例（离线 GPU 机，需已登录私有仓库）：

```bash
docker build -f docker/embedding-runtime.Dockerfile -t <registry>/embedding-runtime:<tag> .
```
