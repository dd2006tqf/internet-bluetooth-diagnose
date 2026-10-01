# 边云完整接入实施设计（Phase 4a 数据面 + 资产桥 + 自动工单 + 知识回流）

> **定位**：把板端 WeakNet 与 `Large-Model-Application` 平台按"第三类可接入面"接通，
> 并按层补齐测试。回答的问题是：**板端的无线事件/区域事故如何成为平台可诊断、
> 可追溯、可触发履约的事实？**
> **状态**：设计草案（brainstorming 产出），**待用户批准后实施**
> **编制**：2026-10-01，基于仓库 HEAD `3f95585`
> **流程决策**（用户拍板）：合并为一个完整任务，**不走 OpenSpec change**；
> 以"分阶段提交 + 每阶段测试 + 契约脚本 + CI 门禁"替代工作流证据链。

---

## 一、已拍板的三个设计决策（2026-10-01 用户确认）

| # | 决策点 | 选择 |
|---|---|---|
| D1 | 事件/事故上行通道 | **独立端点** `POST /network/edge/wireless-events`，沿用现有 Ed25519 验签器；幂等键用 `event_id`/`incident_id`，与遥测的 `(epoch, sequence)` 分域 |
| D2 | 资产对齐范围 | **只桥网关**：`network_assets` ↔ `AssetRecord`（`source_system="weaknet"`）；现场无线设备保持诊断域实体，不入资产池 |
| D3 | incident→工单触发 | 原定「自动开单、默认开」；**P5 实测后修正为「自动开工单草稿、默认开」**——见 §5 的治理边界说明与修正理由 |

范围合并 = 原第三类 S6（资产桥）+ S7（自动开单）+ S8（知识双向），
外加它们的共同前置 Phase 4a（四张表 + 上行）。

---

## 二、现状与缺口（走查结论，2026-10-01）

已建成（消费端，commit `3f95585`）：
- 云端因果诊断引擎：`wireless_contracts`（closed model）、`wireless_rules`（确定性规则）、
  `wireless_diagnosis`（Canonical 决裁 + LLM 润色 + W1~W6 护栏 + 版本化缓存）；
- 诊断出口：`GET/POST /network/.../wireless-incidents/{id}/diagnosis`，结果独立落
  `site_incident_diagnoses`，不回写事实表。

**缺口（生产端全部缺失）**：
1. `_load_bundle()` 默认路径直接 `raise WirelessIncidentNotFound`（`wireless_diagnosis.py:307`
   注释提及"四张 Phase 4a 表"，表**不存在**、迁移**未建**、上行**没有**）——
   诊断接口在真实数据上**永远 404**，只有注入 `bundle_loader` 的测试能跑通。
2. 板端无事件/事故/基线上行（`EdgeTelemetryExporter` 只上报评估快照）。
3. 网关在云端有 `network_assets` 身份，但与售后域 `assets`（`AssetRecord`）**无桥**；
   而平台 `incidents.asset_id` 是 `assets.asset_id` 的外键——**没有桥就永远开不出工单**。
4. 平台 `tests/` 仅 2 文件 566 行，CI 不跑 pytest/ruff/mypy（绿灯无据）。

平台开单的真实链（`persistence/models.py` 实测）：

```
SiteIncident(板端事实)
  → IncidentRecord   必填: asset_id → assets.asset_id(FK), reporter, description, evidence_bundle_id
    → ActionProposalRecord 必填: incident_id(FK), initiator, tool_id, risk_tier, parameters, operation_id
      → WorkOrderRecord   必填: incident_id(FK), proposal_id(FK), creation_mode
```

---

## 三、数据面设计（Phase 4a）

### 3.1 板端 → 云端契约

```jsonc
// POST /network/edge/wireless-events
// Headers: X-Edge-Key-Id / X-Edge-Token / X-Edge-Tenant / X-Edge-Signature（复用 EdgeTelemetryVerifier）
// 签名覆盖发送字节（与遥测同一不变式）
{
  "schema_version": "network.edge.wireless-events.v1",
  "device_id": "radxa-cubie-a7a",
  "watermark_ms": 1770000000000,          // 本次批量已上传到的最大事件时间（游标）
  "events":    [ /* device_events 行，字段同板端表 + event_id */ ],
  "incidents": [ /* site_incidents 行 + evidence_event_ids[]（回链） */ ],
  "baselines": [ /* device_baselines 行 */ ],
  "env_window": {                          // 诊断 bundle 的环境窗口摘要
    "available": true, "link_type": "WIFI", "wifi_anomaly": false,
    "coexistence_warning": false, "from_ms": 0, "to_ms": 0
  }
}
// Response 200
{ "accepted": {"events": n, "incidents": n, "baselines": n},
  "duplicates": {"events": n, "incidents": n, "baselines": n} }
```

- 幂等：`events` 按 `event_id` **ON CONFLICT DO NOTHING**（不可变事实）；
  `incidents` 按 `incident_id` **UPSERT**（state/last_event/resolved_at 会演化）；
  `baselines` 按复合键 UPSERT（与板端同策略）。
- 租户绑定：复用 `X-Edge-Tenant` + 设备归属冲突检查（409 语义与遥测一致）。
- 重试：失败保留在板端游标之后重发；云端幂等使其安全。

### 3.2 云端四张表（alembic `0090_weaknet_wireless_ingestion`）

`network_wireless_events` / `network_site_incidents` /
`network_device_baselines` / `network_env_windows`
（命名与 `IncidentEvidenceBundle` 四类事实一一对应；若与 4b 会话的隐含意图不符，
以本节为准回改注释。）

### 3.3 板端 exporter

- 新组件 `EdgeWirelessUplinkExporter`（复制 `EdgeTelemetryExporter` 骨架：
  独立线程 + 环形缓冲 + 失败保留 + 退避），数据源为 `DatabaseManager`
  （`loadDeviceEventsForReplay` / `querySiteIncidents` / `queryDeviceBaselines`）。
- 游标持久化：`data/wireless-uplink-watermark`（仿 `NetworkEpochStore` 的
  损坏回落语义）。
- **无新增 D-Bus 方法**（读本地 SQLite，不经总线）。

### 3.4 `_load_bundle` 默认实现

按 `incident_id` + tenant 从四张表装载 → `IncidentEvidenceBundle`；
查不到仍 404，但语义从"永远 404"变为"确实未上行"。

---

## 四、资产桥设计（S6，决策 D2）

- 挂接点：`NetworkAssuranceService.ingest_batch` 中网关注册/心跳成功后，
  同事务内 UPSERT `AssetRecord`：
  `source_system="weaknet"`, `source_record_id=network_asset.asset_id`,
  `display_name=display_name`, `model_code="weaknet-gateway"`, `version++`。
- 存量回填：首次部署时对既有 `network_assets` 做一次同步（服务启动钩子或
  `scripts/` 一次性幂等脚本）。
- 现场无线设备（`WirelessDevice`）**不入** `assets`——它们只存在于
  `network_wireless_events` 与诊断视图中（守住"观测对象 ≠ 服务对象"）。

---

## 五、自动开单设计（S7）：实测后修正为「自动开工单草稿」

### 5.1 修正说明（2026-10-01，P5 实测）

原设计（决策 D3：自动开单、配置默认开）在**不改平台治理**的前提下**做不到**。
实测证据（`Large-Model-Application/src/industrial_ops_agent/application/incidents.py:1433`）：

```python
submitted, incident, event = current.submit(
    evidence_confirmed=evidence.status == EvidenceStatus.CONFIRMED.value,
    all_media_clean=bool(media) and all(item.scan_state == ScanState.CLEAN.value for item in media),
    device_authorized=True,
    ...
)
```

创建正式 incident 需要①证据包已确认 ②至少一个 CLEAN 媒体对象；随后开 workorder
还需一条已批准的 `action_proposals`。平台**没有**「系统主体已确认证据」的通道。

因此「自动开单」只有两条实现路径：伪造一份已确认的证据包，或放宽平台证据门禁。
两者都会摧毁这套系统赖以成立的可信度（正是产品定位里反复强调的那条），**均不采纳**。
经用户确认，改为**自动开工单草稿**：

> 自动化止步于**提议**。事故一到，系统自动建一张草稿放进运营方队列；
> 人补证据、确认提交后，平台既有的提交/审批/派工链路照常运转。

### 5.2 实现

- 配置键 `network_auto_incident_draft_enabled`（默认 **true**，与 D3 的「默认开」一致）。
- 触发条件：上行的 `site_incidents` 中**新开**且状态为 `OPEN`/`ONGOING` 的事故。
  `RESOLVED` 不补开——事故已结束，补开只制造噪音。
- 幂等键：`weaknet-incident-draft:<incident_id>`；重复上行不产生第二张草稿。
- 授权与审计：走平台既有 `IncidentDraftService`，主体 `edge-automation`
  以 `FIELD_ENGINEER` 角色（具备 `CREATE_INCIDENT_DRAFT`）、作用域限本租户。
- 草稿挂在**网关资产**上（S6 桥提供身份）。
- 草稿失败绝不冒泡到入库：草稿是下游增强，不能拖垮事实持久化。

### 5.3 定位文档修订草案（`docs/产品定位-工业无线诊断网关.md`，随 P5 提交）

- 第二节「产品不做什么」的「设备管理」行**保持不变**，其下建议新增一行：

  > | 自动建单（平台侧） | 板端仍不控制、不管理任何设备；云端依据已确诊的
  > | `SiteIncident` 自动建一张**草稿**，由人补证据后确认提交。这是诊断结论的
  > | 下游消费，不改变「板端只观测诊断」的边界。 |

## 六、知识双向（S8）

1. **收录（诊断→知识）**：诊断记录持久化成功后，可选地调用平台既有
   knowledge ingest 管道入库（`source="weaknet-diagnosis"`，状态=待人工审核，
   走平台既有审核/索引流程，不绕过治理）。
2. **引用（知识→诊断）**：`_attempt_llm_presentation` 组装 prompt 时，经既有
   knowledge 检索取 top-k 相关条目注入上下文；检索为空/失败时零副作用降级
   （与现有 LLM 失败回退同一纪律）。

---

## 七、三道灵魂拷问

| 维度 | 结论 |
|---|---|
| **D-Bus 契约** | **零新增/零修改 D-Bus 方法与信号**；`com.example.WeakNet.conf` 无需改动。板端只新增读本地库的 HTTP 上行线程 |
| **硬件与环境依赖** | 板端：游标/序列化/批量逻辑 x86 gtest 可闭环；真实上行端到端需板机（沿用 ssh -R 反向隧道 + compose.lite 排练）。云端：全部 pytest hermetic（内存 SQLite，沿用现有两个测试的模式） |
| **多线程并发安全** | 板端新 exporter 独立线程，读库走 `DatabaseManager::mutex_`（既有），与遥测线程无共享可变状态；云端 ingest 走 tenant 事务 + 既有 service 锁纪律；workorder bridge 在 ingest 事务提交后触发（避免锁嵌套） |

---

## 八、分阶段实施（一个任务，八个阶段，每阶段独立提交+测试）

| 阶段 | 内容 | 测试（随阶段提交，先红后绿） |
|---|---|---|
| **P0** | CI 基线：GitHub Actions 加 `cloud-tests` job（pytest tests/ + ruff）；证明**现有** 2 个测试文件绿灯 | CI 本身即证据 |
| **P1** | 云端：四张表迁移 0090 + ingest 端点 + closed-model 契约 + 幂等 | `test_wireless_ingest.py`（签名/幂等/租户/409/422） |
| **P2** | 板端：`EdgeWirelessUplinkExporter` + 游标 + 批量 | `test_edge_wireless_uplink_gtest`（内存 sqlite + 序列化 + 游标推进 + 失败保留） |
| **P3** | `_load_bundle` 默认实现 → 诊断接口端到端可用 | 云端 pytest：四表装载、NOT_FOUND 语义、指纹稳定性；GET/POST 用注入数据跑通 |
| **P4** | 资产桥 + 存量回填 | `test_asset_bridge.py`（UPSERT 幂等、display 同步、回填一次成） |
| **P5** | workorder bridge + 配置键 + **定位文档修订草案提交** | `test_workorder_bridge.py`（OPEN 触发、幂等不开重单、开关关闭不动作、降级路径） |
| **P6** | 知识双向 | `test_knowledge_flow.py`（收录进审核队列、检索注入、空检索降级） |
| **P7** | 端到端排练 + 文档同步（架构设计/产品定位/README 云端章节 + `verify_telemetry_contract.py --strict` 扩展覆盖 wireless-events） | 契约脚本 + 板↔云排练脚本 |

跨阶段横切：**契约脚本每阶段同步扩展**（板端 C++ 结构 / 云端 Pydantic 双侧一致）。

## 九、测试与 CI 的可验收定义（对应"接入后补齐测试"）

- **L1 契约层**：`tools/verify_telemetry_contract.py --strict` 覆盖
  telemetry / action-results / **wireless-events** 全部 schema，进 CI 卡口。
- **L2 连接路径**：`edge/`、`network_assurance/`（含新 ingest/bridge/loader）、
  `persistence` 租户边界 → pytest 行覆盖 ≥90%（pytest-cov 门槛进 CI）；
  板端新增 gtest 全绿并纳入 ctest 正则。
- **L3 平台底盘**：ruff + mypy（`make static`）+ 三条 smoke（错误分类、租户隔离、验签顺序），**不追全域覆盖率**。
- **L4 端到端**：P7 板↔云排练（compose.lite + ssh -R），排练脚本化可重复。

## 十、风险与前置条件

1. **本地测试依赖**：本机无 pip/venv/uv。P1 起本地跑 pytest 需授权安装
   （`sudo apt install python3-pip python3-venv` 或官方 uv 单二进制）；
   未授权则云端测试只能靠 CI 迭代（每次 = 提交+推送+等 CI，显著变慢）。
2. **P5 治理链**：`tool_id` 注册/审批门是否允许无人值守推进——§5 降级条件兜底，
   实施时如实回报实际落点。
3. **与 4b 会话错峰**：本任务由本会话执行（4b 会话已于 `3f95585` 提交收尾、
   工作区干净）；期间不再有第二 writer。
4. **资源**：ARM64 容器编译（板端）与云端 pytest/CI 串行安排。
5. **不做清单**（第一类无事实域）：tts/gpu/supplier/customer_portal 等域
   永不接入；现场设备不入资产池（D2）；`predictive_maintenance` 本轮不接。

## 十一、完成定义（DoD）

1. P0–P7 全部提交且各阶段测试绿（本地 pytest 或 CI，注明来源）；
2. 诊断接口在真实上行数据上 GET/POST 均可用（不再依赖注入 loader）；
3. 一次板↔云排练通过：事件上行 → 诊断 → 资产桥 → 自动工单（或命中降级条件并如实记录）；
4. `verify_telemetry_contract.py --strict` 含 wireless-events 双侧校验并在 CI 通过；
5. 文档同步：产品定位（修订）、架构设计（云端章节）、README 接入状态；
6. 署名/许可口径保持 `tanqf`/MIT（`0c90c45` 之后不得回退）。


---

## 十二、交付状态（截至 2026-10-01）

| 阶段 | 状态 | commit |
|---|---|---|
| P0 CI 基线（pytest + ruff 门禁） | ✅ 完成 | `88acb32` |
| P1 云端四张事实表 + ingest 端点 | ✅ 完成 | `7a37976` |
| P2 板端无线事实上行器（含 `RssiSample` 重名修复） | ✅ 完成 | `c5dfe5c` |
| P3 诊断默认路径从四张表装配（不再永远 404） | ✅ 完成 | `e7edfbb` |
| P4 资产桥（网关入售后资产域） | ✅ 完成 | `a146189` |
| P5 自动开工单草稿（S7，按 §5.1 修正） | ✅ 完成 | `60a6f27` |
| P6 知识双向（S8） | ⬜ 未开始 | — |
| P7 端到端排练 + 文档同步 | ⬜ 未开始 | — |

**门禁现状**：x86 CTest 46/46；云端 pytest 61 passed（`-m "not live"`）+
连接路径 ruff 全绿；GitHub Actions `CI` 三个 job 全绿。

**S8 未开始的理由**：知识检索（`knowledge/retrieval.py`）需要**已发布的索引
release**（`RetrievalQuery.release_id` 来自 release manifest），收录侧
（`KnowledgeIngestionService.create_document`）需要 `PUBLISH_KNOWLEDGE` 权限且
审核分离（创建者不能自审）。这不是一条「加两行」的连接，而是需要先决定：
诊断案例以何种粒度入库、谁来审核、索引 release 由谁发布。建议单独立项。
