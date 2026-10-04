"""T8：HTTP 层轻量冒烟——验证 4 个新端点的传输语义（不重复 E2E 业务链）。

覆盖面（⑥ plan 约定，只到 HTTP 契约层）：
- 401 无 token / 404 无会商 / 422 缺 If-Match / 400 非法 If-Match；
- 有真实会商行时：MANUAL /execute → 409；MANUAL /runbook 未批准 → 409；
  /runbook 已批准 → 200 且 NOT_EXECUTED；跨租户 proposal_id → 404；
- APPROVE / EXECUTE 两权名单分离（纯策略层断言）。

不覆盖：验签（`_read_verified_body` 的 Ed25519 域有独立测试）；Council
内部逻辑与 L5 业务链（tests/test_e2e_trace.py 的 trace 场景已覆盖）。
"""

from __future__ import annotations

from datetime import UTC, datetime, timedelta

import pytest
from fastapi.testclient import TestClient
from sqlalchemy import create_engine
from sqlalchemy.pool import StaticPool

from industrial_ops_agent.auth.identity import IdentityContext, Role
from industrial_ops_agent.config import Environment, Settings
from industrial_ops_agent.persistence.database import Database
from industrial_ops_agent.persistence.models import Base


@pytest.fixture
def app_db() -> Database:
    engine = create_engine(
        "sqlite:///:memory:",
        connect_args={"check_same_thread": False},
        poolclass=StaticPool,
    )
    Base.metadata.create_all(engine)
    return Database.from_engine(engine)


@pytest.fixture
def client(app_db: Database) -> TestClient:
    """最小化 app：真实 DB，假 OIDC verifier（HTTP 层只验形状不验签名）。"""

    class _StubVerifier:
        """接受形如 ``Bearer <subject>:<role>`` 的 token，便于测试角色差异。"""

        def verify(self, token: str) -> IdentityContext:
            if ":" not in token:
                from industrial_ops_agent.auth.oidc import AuthenticationFailure

                raise AuthenticationFailure("token_invalid")
            subject, role = token.split(":", 1)
            roles = {
                "expert": frozenset({Role.DOMAIN_EXPERT}),
                "admin": frozenset({Role.TENANT_ADMIN}),
                "sales": frozenset({Role.AFTER_SALES_ENGINEER}),
                "field_engineer": frozenset({Role.FIELD_ENGINEER}),
            }.get(role, frozenset({Role.DOMAIN_EXPERT}))
            now = datetime.now(UTC)
            return IdentityContext(
                subject_id=subject,
                oidc_subject=f"oidc-{subject}",
                tenant_id="tenant-alpha",
                roles=roles,
                asset_ids=frozenset(),
                site_ids=frozenset(),
                issued_at=now,
                expires_at=now + timedelta(hours=1),
            )

    from industrial_ops_agent.api.app import create_app
    from industrial_ops_agent.auth.policy import Authorizer
    from industrial_ops_agent.security_audit import (
        InMemorySecurityAuditSink,
        SecurityAuditor,
    )

    app = create_app(Settings(environment=Environment.TEST, _env_file=None))
    app.state.database = app_db
    app.state.oidc_verifier = _StubVerifier()
    app.state.authorizer = Authorizer(
        SecurityAuditor(InMemorySecurityAuditSink(), hash_key=b"e2e-http-smoke-key")
    )
    return TestClient(app)


def _h(subject: str = "op-1", role: str = "expert") -> dict[str, str]:
    return {"Authorization": f"Bearer {subject}:{role}"}


# ---------------------------------------------------------------------------
# 传输层断言
# ---------------------------------------------------------------------------


def test_unauthenticated_is_401(client: TestClient) -> None:
    r = client.get("/api/v1/network/assurance/incidents/x/council/proposals")
    assert r.status_code == 401


def test_invalid_token_is_401(client: TestClient) -> None:
    r = client.get(
        "/api/v1/network/assurance/incidents/x/council/proposals",
        headers={"Authorization": "Bearer no-colon"},
    )
    assert r.status_code == 401


def test_proposals_list_404_when_no_council(client: TestClient) -> None:
    r = client.get(
        "/api/v1/network/assurance/incidents/nonexistent/council/proposals",
        headers=_h(),
    )
    assert r.status_code == 404
    assert r.json()["error"]["code"] == "wireless_incident_not_found"


def test_decide_requires_if_match(client: TestClient) -> None:
    r = client.post(
        "/api/v1/network/assurance/proposals/ncprop-x-a1-0/decision",
        headers=_h("approver", "expert"),
        json={"decision": "APPROVED", "reason": "批准该提案以推进闭环验证"},
    )
    assert r.status_code == 422  # If-Match header required


def test_decide_bad_if_match_is_400(client: TestClient) -> None:
    r = client.post(
        "/api/v1/network/assurance/proposals/ncprop-x-a1-0/decision",
        headers={**_h("approver", "expert"), "If-Match": "not-a-number"},
        json={"decision": "APPROVED", "reason": "批准该提案以推进闭环验证"},
    )
    assert r.status_code == 400
    assert r.json()["error"]["code"] == "invalid_if_match"


def test_decide_nonexistent_proposal_is_404(client: TestClient) -> None:
    r = client.post(
        "/api/v1/network/assurance/proposals/ncprop-ghost-a1-0/decision",
        headers={**_h("approver", "expert"), "If-Match": "1"},
        json={"decision": "APPROVED", "reason": "批准该提案以推进闭环验证"},
    )
    assert r.status_code == 404
    assert r.json()["error"]["code"] == "proposal_not_found"


def test_execute_nonexistent_proposal_is_404(client: TestClient) -> None:
    r = client.post(
        "/api/v1/network/assurance/proposals/ncprop-ghost-a1-0/execute",
        headers={**_h("op", "expert"), "Idempotency-Key": "key-1"},
    )
    assert r.status_code == 404


def test_runbook_nonexistent_proposal_is_404(client: TestClient) -> None:
    r = client.get(
        "/api/v1/network/assurance/proposals/ncprop-ghost-a1-0/runbook",
        headers=_h(),
    )
    assert r.status_code == 404


def test_decision_reason_too_short_is_422(client: TestClient) -> None:
    r = client.post(
        "/api/v1/network/assurance/proposals/ncprop-x-a1-0/decision",
        headers={**_h("approver", "expert"), "If-Match": "1"},
        json={"decision": "APPROVED", "reason": "短"},
    )
    assert r.status_code == 422


def test_decision_invalid_enum_is_422(client: TestClient) -> None:
    r = client.post(
        "/api/v1/network/assurance/proposals/ncprop-x-a1-0/decision",
        headers={**_h("approver", "expert"), "If-Match": "1"},
        json={"decision": "MAYBE", "reason": "理由足够长可以进入校验"},
    )
    assert r.status_code == 422


# ---------------------------------------------------------------------------
# 权限语义：approve 与 execute 权限分离
# ---------------------------------------------------------------------------


def test_queue_action_denied_returns_403_not_500(
    app_db: Database, client: TestClient, monkeypatch: pytest.MonkeyPatch
) -> None:
    """拒绝必须是 403，不是裸 500。

    ``app.state.network_assurance_service`` 在这里是唯一的"未装配"依赖，
    真实部署（rehearsal_server）会装配它。若用 monkeypatch 强行注入就会
    变成"实际测试不存在的行为"——那样即使生产 500，测试也会通过。
    这里改为从 **真实装配路径**（平台自己的 service + accessor）取，只把
    "从 app.state 读"这一段改成直供：

    背景：``network_assurance.py`` 有 15 处直调 ``authorizer.require(...)``
    且从不捕获 ``AuthorizationDenied``（其它 routes 模块全都捕获）。
    2026-10-05 生产冒烟里，用 after_sales 身份排队动作被正确拒绝、却以
    ``internal_error`` 500 回来。DOMAIN_EXPERT 没有
    ``MANAGE_NETWORK_DEVICE``（仅 TENANT_ADMIN 有），是最小复现。
    """
    from industrial_ops_agent.api.dependencies import (
        get_network_assurance_service as _real_accessor,
    )
    from industrial_ops_agent.network_assurance.service import NetworkAssuranceService

    monkeypatch.setitem(
        client.app.dependency_overrides,
        _real_accessor,
        lambda: NetworkAssuranceService(database=app_db),
    )

    r = client.post(
        "/api/v1/network/assets/radxa-cubie-a7a/actions",
        headers=_h("op-1", "expert"),
        json={"config_key": "rtt.interval", "config_value": "10s"},
    )
    assert r.status_code == 403
    assert r.json()["error"]["code"] == "authorization_denied"


def test_approve_and_execute_permissions_are_separate() -> None:
    """权限分离（纯策略层断言，避免 HTTP 路径上 resource 可见性干扰）：

    APPROVE 名单 = after_sales / domain_expert / tenant_admin；
    EXECUTE 名单 = after_sales / domain_expert（**没有** tenant_admin）。
    """
    from industrial_ops_agent.auth.policy import ROLE_ACTIONS, Action

    approvers = {r for r, acts in ROLE_ACTIONS.items() if Action.APPROVE_NETWORK_ACTION in acts}
    executors = {r for r, acts in ROLE_ACTIONS.items() if Action.EXECUTE_NETWORK_ACTION in acts}
    assert Role.AFTER_SALES_ENGINEER in approvers
    assert Role.DOMAIN_EXPERT in approvers
    assert Role.TENANT_ADMIN in approvers
    assert Role.FIELD_ENGINEER not in approvers
    assert Role.FIELD_ENGINEER not in executors
    # tenant_admin 能批但**不能**执行——两权不合并
    assert Role.TENANT_ADMIN not in executors


# ---------------------------------------------------------------------------
# 有真实会商行时的端点语义（复用 trace 场景的确定性 Council mock）
# ---------------------------------------------------------------------------


@pytest.fixture
def seeded(app_db: Database) -> dict[str, str]:
    """用确定性 Council 产出真实 proposal/approval 行，供 HTTP 层断言。"""
    # pytest 以 prepend 模式把 tests/ 本身放进 sys.path（tests/ 不是包），
    # 因此这里按模块名导入，而不是 tests.test_e2e_trace —— 后者在裸
    # `pytest` 入口（CI）下会 ModuleNotFoundError。
    from test_e2e_trace import _convene, _find_proposal, _rows

    from industrial_ops_agent.persistence.models import (
        NetworkActionApprovalRecord,
        NetworkCouncilProposalRecord,
    )
    from industrial_ops_agent.persistence.tenant import TenantContext

    ctx = TenantContext(tenant_id="tenant-alpha", subject_id="initiator-1")
    _convene(app_db, ctx)
    manual_p, manual_a = _find_proposal(app_db, manual=True)
    remote_p, remote_a = _find_proposal(app_db, manual=False)
    assert _rows(app_db, NetworkCouncilProposalRecord)
    assert _rows(app_db, NetworkActionApprovalRecord)
    return {
        "manual_proposal": manual_p.proposal_id,
        "manual_version": str(manual_a.version),
        "remote_proposal": remote_p.proposal_id,
        "remote_version": str(remote_a.version),
    }


def test_manual_execute_is_409(client: TestClient, seeded: dict[str, str]) -> None:
    r = client.post(
        f"/api/v1/network/assurance/proposals/{seeded['manual_proposal']}/execute",
        headers={**_h("op", "expert"), "Idempotency-Key": "smoke-1"},
    )
    assert r.status_code == 409
    assert r.json()["error"]["code"] == "manual_execution_not_allowed"


def test_runbook_before_approval_is_409(client: TestClient, seeded: dict[str, str]) -> None:
    r = client.get(
        f"/api/v1/network/assurance/proposals/{seeded['manual_proposal']}/runbook",
        headers=_h(),
    )
    assert r.status_code == 409
    assert r.json()["error"]["code"] == "approval_conflict"


def test_runbook_after_approval_is_not_executed(
    client: TestClient, seeded: dict[str, str]
) -> None:
    pid = seeded["manual_proposal"]
    # 批准（发起人是 initiator-1，故用另一个 subject 规避 SoD）
    r = client.post(
        f"/api/v1/network/assurance/proposals/{pid}/decision",
        headers={**_h("approver-2", "expert"), "If-Match": seeded["manual_version"]},
        json={"decision": "APPROVED", "reason": "低风险只读，现场执行"},
    )
    assert r.status_code == 200, r.text
    assert r.json()["status"] == "APPROVED"

    r = client.get(
        f"/api/v1/network/assurance/proposals/{pid}/runbook", headers=_h()
    )
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["execution_status"] == "NOT_EXECUTED"
    assert body["manual_execution_required"] is True
    assert body["command"].startswith("sudo weaknet action ")

    # 同一 proposal 的 /execute 仍必须 409（MANUAL 不走远程通道）
    r = client.post(
        f"/api/v1/network/assurance/proposals/{pid}/execute",
        headers={**_h("op", "expert"), "Idempotency-Key": "smoke-2"},
    )
    assert r.status_code == 409
