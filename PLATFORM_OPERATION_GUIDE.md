# WeakNet 边云协同网络智能诊断平台 — 运行与演示全景指南

本项目由“tanqf”开发。

本项目实现了从**硬件底层内核探针 (eBPF + C++)** 到**中心云工业大模型智能诊断平台 (FastAPI + PostgreSQL + 大模型推理网关)** 的端到端完整闭环。本文档汇总当前真实的部署拓扑、启动命令与全流程演示操作指南。

---

## 一、 系统架构与组件拓扑

```
+-----------------------------------------------------------------------------------+
|                    【中心云 / 云端服务器】(Ubuntu 24.04, 81.71.76.133)              |
|                                                                                   |
|   1. 网络保障云端服务 (systemd: weaknet-cloud-api)                                 |
|      - 入口: Large-Model-Application/scripts/rehearsal_server.py                  |
|      - 监听: 0.0.0.0:8000，前缀 /api/v1                                          |
|      - PostgreSQL (weaknet-postgres 容器, 127.0.0.1:5432/industrial_ops)          |
|      - .env 提供 IOAP_MODEL_GATEWAY_*（UpstreamCouncilClient 直连中转站）          |
|                                                                                   |
|   2. 构建容器 (常驻)                                                              |
|      - weaknet-arm64-dev：ARM64 QEMU 模拟编译环境                                 |
|      - 绑定挂载仓库 → /src                                                       |
|                                                                                   |
|   3. 反向隧道落点                                                                 |
|      - 127.0.0.1:2222  ← 板端 cloud-tunnel.service 主动拨入                        |
|      - ssh board 即可登板（~/.ssh/config 已配别名）                                |
+-----------------------------------------------------------------------------------+
                                      ▲
                                      │ 上行遥测/事件 (Ed25519 签名)
                                      │ 下行控制回执 (Pull-on-Upload 随路拉取)
                                      ▼
+-----------------------------------------------------------------------------------+
|                        【边缘端 / Radxa Cubie A7A 开发板】                         |
|                                                                                   |
|   4. WeakNet 内核网络监测服务 (systemd: weaknet-server)                            |
|      - 10 个 eBPF 内核级探针（详见 docs/架构设计.md）                                |
|      - C++ 弱网多维评估引擎（SLE 评估矩阵，每 15 秒发布不可变评估快照）               |
|      - 本地 SQLite 历史持久化 (/home/radxa/weaknet/data/history.db)                |
|      - 遥测上报器 (EdgeTelemetryExporter: libcurl + Ed25519 签名)                  |
|      - 网络代次持久化 (NetworkEpochStore: 消除重启后上行遥测静默丢弃)                |
|      - 下行控制回执 (Pull-on-Upload 随路拉取 + 白名单热更新 + 签名回执)              |
|                                                                                   |
|   5. 反向隧道服务 (systemd: cloud-tunnel.service)                                |
|      - ssh -R 2222:localhost:22 → 云端，Restart=always 自动重连                    |
+-----------------------------------------------------------------------------------+
```

### 关键路径前缀

云端 API 所有路由挂载在 `/api/v1` 前缀下（`rehearsal_server.py` / `api/app.py` 均以此 prefix 挂载）。板端上行地址为 `http://<云端>:8000/api/v1/network/edge/*`。

---

## 二、 网络连通方案（关键前置条件）

### 拓扑背景
- **开发板** 位于家庭局域网（NAT 后），无公网地址，云端无法主动连入。
- **云端服务器** 是公网可达的轻量云主机（`81.71.76.133`），板子有正常出网能力。
- **方案**：板端 `cloud-tunnel.service` 主动建立 `ssh -R 2222:localhost:22` 反向隧道，云端通过 `127.0.0.1:2222` 访问板端 SSH。隧道只绑定回环地址，不对公网暴露。

### 隧道服务（板端）

```ini
# /etc/systemd/system/cloud-tunnel.service
[Unit]
Description=Reverse SSH tunnel to cloud dev server (cloud-dev:2222 -> board:22)
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=radxa
ExecStart=/usr/bin/ssh -o BatchMode=yes -o StrictHostKeyChecking=no \
    -i /home/radxa/.ssh/id_cloud_tunnel \
    -o ServerAliveInterval=30 -o ServerAliveCountMax=3 \
    -o ExitOnForwardFailure=yes -o TCPKeepAlive=yes \
    -N -R 2222:localhost:22 ubuntu@81.71.76.133
Restart=always
RestartSec=10

[Install]
WantedBy=multi-user.target
```

### 云端访问板端

```bash
ssh board                          # ~/.ssh/config 已配 Host board → 127.0.0.1:2222
ssh board 'systemctl status cloud-tunnel.service'   # 检查隧道服务状态
ssh board 'sudo systemctl restart cloud-tunnel.service'  # 重启隧道
```

---

## 三、 一键启动与运行

### 1. 云端服务（systemd 管理）

```bash
# 查看状态
sudo systemctl status weaknet-cloud-api.service

# 重启（如配置/代码变更后）
sudo systemctl restart weaknet-cloud-api.service

# 查看日志
sudo journalctl -u weaknet-cloud-api.service -f
```

服务单元位于 `/etc/systemd/system/weaknet-cloud-api.service`，以 `User=ubuntu` 运行，工作目录为 `Large-Model-Application/`，通过 `EnvironmentFile=Large-Model-Application/.env` 注入模型网关配置。

### 2. 编译并部署开发板边缘端

```bash
# 一键完成：ARM64 容器内增量编译 + 打包产物 + rsync 到板端 + systemd 重启
./tools/ci.sh --skip-test
```

*注：板端服务启动后，若需手工查验板端状态：*
```bash
ssh board 'sudo systemctl status weaknet-server'
```

### 3. 验证云端服务

```bash
# API 文档（路由清单）
curl -s http://localhost:8000/openapi.json | python3 -c "import json,sys; d=json.load(sys.stdin); print('routes=',len(d['paths']))"

# Swagger UI
open http://localhost:8000/docs
```

---

## 四、 平台演示全流程指南（建议演示步骤）

### 演示一：边云遥测不可变上报

板端每 10 秒采集一次遥测数据，通过 Ed25519 签名后上行到云端 `/api/v1/network/edge/telemetry`。

```bash
# 查看板端遥测上行日志
ssh board 'sudo journalctl -u weaknet-server -n 20 --no-pager | grep -i "telemetry\|edge"'

# 查看云端接收日志
sudo journalctl -u weaknet-cloud-api.service -n 20 --no-pager | grep -i "telemetry\|edge"
```

### 演示二：因果护栏大模型排障助手 (Copilot)

1. 云端服务已配置 `IOAP_MODEL_GATEWAY_*` 环境变量（`.env`），`UpstreamCouncilClient` 直连中转站（`https://vectide.cn/v1`）。
2. 调用 Council 接口（需真实 incident_id）：

```bash
# 列出可用 incident
# （需先有真实 incident 数据，见演示三）
curl -s http://localhost:8000/api/v1/network/assurance/incidents/{incident_id}/council \
  -H "Authorization: Bearer <token>"
```

3. Council Runner 会调用真实模型（当前 `deepseek-v4-pro-0813`），产出 3 位专家意见（RF/Kernel/Ops）+ closed-schema ActionProposal 列表。
4. fail-closed 语义：key 缺失、401、截断 JSON 等异常 → `CouncilFailure(FAILURE_MODEL)`，绝不产出半合法建议。

### 演示三：边云下行控制通道（参数热调优与回执闭环）

1. 在设备详情页或 API 中对 `radxa-cubie-a7a` 下发控制：

```bash
# 通过 API 直接入队（direct queue，需 MANAGE_NETWORK_DEVICE，该权限仅在 TENANT_ADMIN 名单上）
# 注意：排练身份已收回 TENANT_ADMIN，此路由当前返回 403 authorization_denied；
#      审批链改用 POST /api/v1/network/assurance/proposals/{proposal_id}/execute
curl -X POST http://localhost:8000/api/v1/network/assets/radxa-cubie-a7a/actions \
  -H "Content-Type: application/json" \
  -d '{"config_key": "rtt.interval", "config_value": "5s"}'
```

2. **终端实时查验（双屏见证）**：
   - 边缘端在下一次遥测响应中随路拉取指令，经白名单校验后生效：
     ```bash
     ssh board 'sudo /home/radxa/weaknet/client/bin/weaknet-cli get rtt'
     ```
   - 查看板端日志：
     ```bash
     ssh board 'sudo journalctl -u weaknet-server -n 10 --no-pager | grep "已应用"'
     ```
   - 边缘端签名后异步回执 `/api/v1/network/edge/action-results`，云端 `network_pending_actions` 状态变为 `APPLIED`，同时落 `network_action_outcomes` 投影行。

3. 完整审计链可通过 `test_e2e_trace.py` 验证（需板端上行有真实 incident 数据）。

---

## 五、 常见问题与故障排查

### 1. `ssh board` 连不上（Connection refused）

**原因**：板端 `cloud-tunnel.service` 反向隧道未建立或已断开。

**排查**：
```bash
# 在板子上检查隧道服务
systemctl status cloud-tunnel.service
journalctl -u cloud-tunnel.service -n 30

# 检查板子出网是否正常
curl -s -o /dev/null -w "%{http_code}" http://81.71.76.133
```

**恢复**：
```bash
sudo systemctl restart cloud-tunnel.service
```

### 2. 开发板重启后云端数据库没有新数据

- **原因**：开发板 `sequence_id` 进程计数器从 1 重启，若 `network_epoch` 也是 1，云端会将 `(epoch=1, seq=1..N)` 误判为历史重复项而静默丢弃。
- **状态**：**已彻底修复**。开发板引入了 `NetworkEpochStore`，重启时代次严格持久化递增（1 -> 2 -> 3...），彻底杜绝静默去重问题。

### 3. 开发板本地 history.db 打不开

- **原因**：`weaknet-server.service` 以 root 运行但 `data/` 目录属于 `radxa:radxa`，缺少 `CAP_DAC_OVERRIDE` 导致 root 被当成 other 用户拒绝写入。
- **状态**：**已彻底修复**。服务单元已固化添加 `CAP_DAC_OVERRIDE` 权能。

### 4. 云端服务起不来（ImportError: cannot import name ...）

- **原因**：`rehearsal_server.py` 是云端当前入口，其依赖 shim 需手动补齐新增的 service accessor。路线图③④⑤ 的新路由依赖 `get_risk_prediction_service` / `get_network_action_approval_service` / `get_network_council_service` 三个 accessor。
- **状态**：**已彻底修复**（commit `0975e42`）。同时已注册平台统一错误边界 `register_error_handlers`，404/403 不再退化为裸 500。

### 5. Council 接口报 `CouncilModelResponseError` / `FAILURE_MODEL`

- **原因**：模型网关未配置或上游返回非法响应。
- **排查**：检查 `.env` 中 `IOAP_MODEL_GATEWAY_API_KEY` 是否有效；检查 `sudo journalctl -u weaknet-cloud-api.service` 中是否有 401/超时/JSON 截断日志。
- **语义**：fail-closed 是**设计行为**——宁可无建议，也不产出半合法输出。

---

## 六、 遗留事项与已知边界

- **演示二/三的完整 e2e 链**（incident → diagnosis → council → proposal → approval → pending_action → outcome）已用**真实蓝牙断连事件**跑通上游：`network_wireless_events`/`network_site_incidents`/`site_incident_diagnoses` 均有真实行（2 台 BLE 设备亚秒级同步断开 → `COEXISTENCE_RF_INTERFERENCE` 诊断）。
- **远端腿（CONFIG_CHANGE → REMOTE_PENDING_ACTION）需可控 fixture**：真实模型在超时类 incident 上只产出 `ACTION_ID`（写死映射 `ACTION_ID → MANUAL_RUNBOOK`），2026-10-08 三次真实会商（attempt 1 失败 / attempt 2、3 成功）均未产出 `CONFIG_CHANGE`。当前库中唯一的 REMOTE 链是**人工注入的 fixture**：`proposal_id='ncouncil-10d3ed70cf6749b1-remote-config'`（`proposal_index=10`、`proposal_digest` 为占位串 `sha256:remote-config-digest`、不在 council 落库的 `proposals_json` 内）。**不要把该链当作真实 Council 闭环证据**。
- **真实 Council 会商脆弱性**：`recommendation_direction` 契约上限 1000 字符（schema 已写入 system prompt），模型仍会偶发超长 → `COUNCIL_SCHEMA_PARSE_FAILED` → fail-closed 409（10-07 五次会商中四次失败）；另有 `COUNCIL_MODEL_FAILED`（上游网关/网络抖动）。重跑 `?force=true` 通常可成功（10-08 实测 3 次中 1 次失败）。
- **网关目录版本漂移防线已接通**：板端自述动作目录指纹（当前 `bdb093ac5310`）随无线事件上行；云端落库并进会商输入，不一致则 Policy 对**所有**提案 fail-closed（`catalog_version_mismatch`）、零审批行。缺失=不阻断。契约与真机演示记录见 `docs/网关动作目录版本契约.md`。注意：**上线新白名单/动作而网关未同步更新时会立刻全线拦截**——这正是设计意图，但发布顺序需先网网关、后云端。
- **模型网关**已配置完成（`test_network_council_live.py` PASSED），Council 链路已跑过真实 incident（见上）。
- **排练身份**当前为正式生产最小权限 `{AFTER_SALES_ENGINEER}`；临时的 `TENANT_ADMIN` 已收回（`MANAGE_NETWORK_DEVICE` 仅在该角色名单上，故 direct queue `/network/assets/{id}/actions` 与 `POST /network/copilot/config` 现在返回 403，属预期）。
- **`start_platform.sh`** 是遗留的一键拉起脚本，拉起的是 `compose.lite.yaml` 全套微服务栈（PostgreSQL/Keycloak/Vault/Temporal/Web 等）。当前实际生产入口是 `rehearsal_server.py`，两者不冲突但用途不同：前者是完整开发环境，后者是生产/排练切片。
