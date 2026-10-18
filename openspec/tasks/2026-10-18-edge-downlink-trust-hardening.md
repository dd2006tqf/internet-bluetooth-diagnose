# 下行下发信任加固与权限闭环（S1/S2/S3/S7/S8）

> 日期：2026-10-18
> 分类：快速修复
> 结果：已完成

## 构思

全仓 code review 发现下行控制平面存在链条级信任缺口：

- **S1（Important）**：板端 `applyPendingActions` 过去只认响应中的 JSON 字段，
  无 detached 签名验证；且 `generation` 仅在 `trialable || v2_metadata` 时
  校验，普通配置键缺 generation 仍直通 `setMonitorParam`；`edge.url` 允许
  任意非回环明文 HTTP，令牌与签名私钥有链路暴露风险。
- **S2（Important）**：云端 `/api/v1/network/assets/{id}/actions` 直连接口
  过去只要求 `MANAGE_NETWORK_DEVICE`（`tenant_admin` 拥有的读/管设备权），
  且对 `config_key` 无白名单限制，可绕过 Council 审批链直接下发非 trialable 键。
- **S3（Minor）**：执行权 `EXECUTE_NETWORK_ACTION` 游离在 `OPA_ENFORCED_ACTIONS`
  强制集之外。
- **S7（Minor）**：TRIAL 看门狗过去只要 `overall != BAD` 就判定健康转正
  （`confirmStable`），把 `UNKNOWN`（证据不足）误当良性背书。
- **S8（Minor）**：遥测与无线上行 422 校验失败日志过去截断打印原始载荷明文，
  可能暴露敏感字段。

## 实现方式

### 板端（WeakNet C++）
- `server/include/weaknet_config.hpp` + `server/src/weaknet_config.cpp`
  - 新增 `edge.cloud_public_key_path`（Ed25519 PEM 路径，可选）；
  - 新增 `edge.allow_insecure_http`（默认 false，全文件解析完后校验：
    非回环地址且 scheme 为 http 则拒绝加载启动）；
  - `ConfigTransaction`：支持显式配置窗口（单测以 0s 立即到期）；
    `TrialWindow` 新增 `deadline_extended` 标志；
    新增 `extendDeadline()` 供 UNKNOWN 延长一次（原 deadline + window，
    最多 2× 窗口）。
- `server/include/edge_telemetry_exporter.hpp` + `server/src/edge_telemetry_exporter.cpp`
  - S1#1：配置了 `cloud_public_key_path` 时，调用 `verifyPendingActionsSignature`
    对响应内 `pending_actions` 数组原始字节（含 `[ ]`）验签，失败或缺签名
    整批拒绝（`stats_.unsigned_responses_rejected++`）；
  - S1#2：`applyPendingActions` 门禁收紧为**所有**动作一律强制
    `generation_present && generation > 0`；
  - S7：`evaluateTrialDeadline` 确认条件收紧为 `overall == GOOD && reach != BAD`；
    `UNKNOWN` 且未延长过则调 `extendDeadline()` 延长一次试用；延长耗尽仍非
    GOOD 保守回滚。
- `server/test/unit/test_weaknet_config_gtest.cpp`
  - 新增非回环明文 HTTP 拒绝、`allow_insecure_http: true` 放行、回环地址放行测试。
- `server/test/unit/test_edge_telemetry_exporter_gtest.cpp`
  - 新增普通动作缺 generation 拒绝、未签名整批拒绝、篡改验签失败整批拒绝；
  - 新增 GOOD 转正、DEGRADED 回滚、UNKNOWN 延长一次后回滚、延长期内 GOOD 转正测试。

### 云端（Large-Model-Application Python）
- `Large-Model-Application/src/industrial_ops_agent/secrets.py`
  - 新增 `SecretName.NETWORK_EDGE_DOWNLINK_SIGNING_PRIVATE_KEY`（环境变量
    `IOAP_NETWORK_EDGE_DOWNLINK_SIGNING_PRIVATE_KEY`）。
- `Large-Model-Application/src/industrial_ops_agent/config.py`
  - 新增 `network_edge_downlink_signing_key_id`。
- `Large-Model-Application/src/industrial_ops_agent/network_assurance/signing.py`
  - 新增 `load_ed25519_private_key()` 与 `EdgeDownlinkActionSigner`（`sign_actions`
    以 Starlette wire 相同 compact JSON `separators=(",", ":")` 产出签名）。
- `Large-Model-Application/src/industrial_ops_agent/api/dependencies.py` +
  `api/app.py` + `runtime.py`
  - 依赖注入 `get_edge_downlink_signer`；未配密钥返回 None（平滑迁移）。
- `Large-Model-Application/src/industrial_ops_agent/api/routes/network_assurance.py`
  - `EdgeTelemetryAcceptedResponse` 增加 `pending_actions_signature` 与
    `pending_actions_key_id` 可选字段；
  - `ingest_edge_telemetry` 在下发动作非空且 signer 就绪时对 actions 数组签名；
  - S2：`queue_network_action` 权限从 `MANAGE_NETWORK_DEVICE` 升为
    `EXECUTE_NETWORK_ACTION`；且必须命中 `is_known_config_key` 白名单，
    否则 409 `edge_config_key_requires_approval`；
  - S8：两处 422 日志改记 `sha256(raw)[:16]`、bytes 与 error_count，不打明文。
- `Large-Model-Application/src/industrial_ops_agent/auth/policy.py` + `infra/opa/policy.rego`
  - S3：`Action.EXECUTE_NETWORK_ACTION` 加入 `OPA_ENFORCED_ACTIONS`；
    rego 补入 `after_sales_engineer` 与 `domain_expert` 的角色动作表。
- 测试：新增 `test_edge_downlink_trust.py`（6 用例）；更新
  `test_e2e_http_smoke.py` 与 `test_action_policy.py`（38 用例全绿）。

## 结果

已完成。下行链路闭环：响应有签名、板端有验签、代次全强制、直连接口收紧
至白名单+执行权、看门狗 UNKNOWN 延迟回滚、日志无明文泄露。

验证：
- C++：x86 `ctest` 52/52 套件通过；
- Python：pytest 38 + 35 用例全绿。
