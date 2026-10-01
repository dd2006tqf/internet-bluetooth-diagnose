# P6 知识双向设计（S8：诊断 ⇄ 知识库）

> **状态**：设计草案，**待用户批准后实施**
> **编制**：2026-10-01，基于 HEAD `dab027f`
> **已定决策**：收录=**事故结案时自动建草稿**；发布=**人工手动发 release**；
> 创建权限=**新增窄 action**；文档内容=**摘要+解释**（LLM 报告不入库）；
> source_uri=**平台基址配置**；发布链=**有 Temporal，按平台原设计**；
> 角色=**沿用 FIELD_ENGINEER**（不新建窄角色）。

---

## 一、走查结论（决定设计形态的六条实测事实）

| # | 事实 | 依据 |
|---|---|---|
| F1 | 创建知识文档需要 `PUBLISH_KNOWLEDGE`（仅 DOMAIN_EXPERT / TENANT_ADMIN），而审核分离要求创建者≠审核者 | `knowledge/ingestion.py:581` `_require_manage`；`:262` `knowledge_review_separation_required` |
| F2 | 检索必须绑定一个 **PUBLISHED + is_active** 的 release，且 query 需 `device_model` 字符串 | `retrieval.py:124` `release_status="PUBLISHED"`；`models.py:10-19` |
| F3 | 发布链是异步的：`build_release` → EVALUATION_PENDING →（Temporal 评估）→ CANDIDATE → promote → PUBLISHED | `ingestion.py:359`、`index_evaluation.py:247`、`knowledge/service.py:63` |
| F4 | ACL 四类是 **AND** 组合；空列表 = 该维度不限制；`classification="restricted"` 必须给角色 | `retrieval.py:228-245`；`ingestion.py:1019-1022` |
| F5 | `source_uri` 强制 `https/s3/minio`，诊断记录没有 URL | `ingestion.py:1055-1063` |
| F6 | `IncidentDraftService.create` 走 `lock_subject`，要求 `SubjectRecord.status == "active"` 且成员存在；而**全仓没有创建 edge 主体的代码** | `review_isolation.py:89`；`grep SubjectRecord(` 仅 seed / tenant_admin |

**F6 的连带风险（登记，不在本次修）**：`seed.py` 写 `status="active"`，
`tenant_administration/service.py:393` 写 `status="ACTIVE"`，而 `lock_subject`
只认 `"active"`。**经管理员流程创建的成员在 P5/P6 的草稿路径上会被判
`subject_unavailable`**。本次按 seed 口径（小写）provision 自动化主体，
并把该不一致作为遗留登记。

---

## 二、P6a：权限与配置地基

1. **新增动作** `Action.CREATE_KNOWLEDGE_DRAFT = "knowledge_draft.create"`。
2. **授权给 `FIELD_ENGINEER`**（用户决策：沿用现有角色）。同时给
   `DOMAIN_EXPERT` / `TENANT_ADMIN`，保证人工路径能力不缩水。
3. **不加入 `OPA_ENFORCED_ACTIONS`**，与既有 `CREATE_INCIDENT_DRAFT` 的先例一致
   （rego 里刻意不列该动作，人工创建 incident 草稿本就只走角色表）。rego 加注释。
4. **创建与管理分离**：`_create_version` 增加一个"所需动作"参数——
   `create_document` → `CREATE_KNOWLEDGE_DRAFT`；
   `create_version`（给既有文档加版本）→ 仍 `PUBLISH_KNOWLEDGE`。
   这样自动化只能**建新草稿**，不能改已审定的文档。
5. **自动化主体的唯一 provision 点**：`network_assurance/automation_subject.py`
   upsert `SubjectRecord(tenant, "edge-automation", oidc_issuer="platform-internal",
   oidc_subject="edge-automation", status="active")`（小写，对齐 `lock_subject`）。
   P5 的草稿工厂一并改用它（顺带修掉 P5 的潜在失败）。
6. **配置** `network_public_base_url`（默认空）+ 校验（https、无凭据/查询/片段，
   与 `_validate_source_uri` 同规则）。**为空则跳过自动建草稿并告警**——
   不伪造 URL（用户决策）。

## 三、P6b：收录侧（事故结案 → 知识草稿）

- 触发：上行中事故**转为 RESOLVED**（含首次即 RESOLVED 的历史补发）。
- 幂等：`weaknet-knowledge-draft:<incident_id>`；重复上行不产生第二篇。
- 内容（`network_assurance/knowledge_bridge.py` 纯函数，可单测）：
  事故元信息（现场/网关/设备数/时间窗）+ 确定性结论
  （pattern / hypothesis / confidence / reasons）+ 一段解释性建议。
  **LLM 报告不入库**（用户决策：它是幻觉面）。
- 分类与作用域：`classification="internal"`；
  `acl_roles=("field_engineer","domain_expert","after_sales_engineer")`；
  `device_families=()`、`device_models=()`（空 = 不限制适用性，
  因为这是"区域无线异常案例"，不绑定具体设备族）。
- `source_uri = <network_public_base_url><api_prefix>/network/assurance/incidents/<id>/diagnosis`
  ——指向平台真实存在的诊断资源。
- 与 P5 同一纪律：**草稿失败绝不冒泡到上行**。

## 四、P6c：引用侧（知识 → 诊断提示词）

- `get_diagnosis` / `diagnose_incident` 增加可选 `identity`（默认 None →
  不检索，既有调用与测试零改动）。
- 检索：取该租户 **PUBLISHED + is_active** 的 release；`RetrievalQuery`
  `device_model="weaknet-gateway"`、`device_family=None`、roles 取 identity 角色；
  `HybridRetriever.search()`。任何异常（无 release / 检索失败）→ **空知识继续**，
  与既有 LLM 失败回退同一纪律，绝不阻断诊断。
- **护栏纪律（关键）**：检索到的文本只作为**参考材料**进入 LLM 的用户载荷，
  用显式标签隔开；它**绝不进入 `evidence_catalog` / `valid_evidence_ids`**，
  因此 W6 的"事实断言必须绑定证据 ID"对知识文本依然闭合——
  模型不能把知识库内容冒充为本案证据。
- 路由：`GET/POST diagnosis` 把 `identity` 透传（路由本就持有）。

## 五、测试（分层，全部 hermetic）

| 文件 | 覆盖 |
|---|---|
| `test_knowledge_draft_action.py` | 新动作授给谁 / 未授角色被拒 / create_document 走新动作（create_version 不受影响） |
| `test_knowledge_bridge.py` | 内容构造（字段齐全、无 LLM 报告）；RESOLVED 建、OPEN 不建；幂等不重复；缺 base URL 时静默跳过；创建者=edge-automation（审核分离前提） |
| `test_knowledge_citation.py` | 有已发布 release（按 `seed.py` 形态造夹具：PUBLISHED+is_active+chunks）→ 命中并进入 LLM 载荷；无 release / 无 identity / 后端失败 → 不报错不检索；角色 ACL 排除；**知识文本从不进入 evidence_catalog**；W6 拒绝知识 id 当证据 |
| （主体 provision 并入 `test_knowledge_draft_action.py`） | provision 幂等、status 小写（`lock_subject` 只认小写） |

夹具说明：release/version/chunk 三表按 `knowledge/seed.py` 的方式直接构造，
**不改生产门禁**（build→evaluate→promote 仍需 Temporal，已在部署中具备）。

## 六、明确不做的

1. **不自动发 release**（用户决策：人工 promote）。
2. **不把知识检索结果当证据**（不进 evidence_catalog / 不参与判定）。
3. **不做知识版本漂移检测**：本次是"读当前 active release"，不是"诊断时读了哪版"
   的审计；若将来需要，另立 change。
4. **不修 F6 的 status 大小写不一致**（登记为遗留，交由你决定是否立项）。

## 七、分阶段与验收

| 阶段 | 内容 | 门禁 |
|---|---|---|
| P6a | 动作/授权/创建分离/主体 provision/配置 | pytest 全绿 + 连接路径 ruff |
| P6b | 收录侧 + 内容构造 | 同上 + CI |
| P6c | 引用侧 + 路由透传 | 同上 + CI |

**DoD**：① 事故结案自动产生一篇待审知识草稿（人审后人发 release）；
② 诊断在存在 active release 时能带着知识参考做解释，且护栏仍只认证据 ID；
③ 无 release / 无配置时全链路安全降级、零异常。

## 八、交付状态（截至 2026-10-02）

| 阶段 | 状态 | commit |
|---|---|---|
| P6a 权限与配置地基 | ✅ 完成 | `2e11dce` |
| P6b 收录侧（结案 → 知识草稿） | ✅ 完成 | `cbd0b1c` |
| P6c 引用侧（知识 → 解释背景） | ✅ 完成 | `32e473e` |

**门禁现状**：云端 pytest **87 passed + 1 deselected**（含 P6 全部 19 例新测试）；
门禁路径 ruff clean；契约脚本 OK；x86 ctest 46/46；GitHub Actions
`2e11dce`、`cbd0b1c` 两次 CI 全绿（`32e473e` 见当次运行）。

**留给人工的两步（本设计刻意不自动化）**：
1. 事故结案产生的 DRAFT 由 DOMAIN_EXPERT 审核（创建者=edge-automation，
   平台审核分离强制换人）；
2. `build_release` → 评估（Temporal）→ promote，由人发布 release——
   引用侧才检索得到。
