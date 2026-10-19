# tests/test_opa_token_auth.py
# S4：OPA REST API 共享密钥 token 认证（板↔云决策通道加固）
#
# 覆盖三件事：
#   1. OpaPolicyClient 在给定 token 时携带 Authorization: Bearer 头、
#      缺省时不带头（未加固本地实例兼容）；
#   2. SecretName.OPA_TOKEN 的环境变量映射与 OPA 容器同名共用；
#   3. LazySecretProvider 的「Vault 主查 + 环境变量兜底」仅对豁免键生效。

from __future__ import annotations

import httpx
import pytest

from industrial_ops_agent.auth.opa import (
    OpaDecisionInput,
    OpaPolicyClient,
    OpaUnavailable,
)
from industrial_ops_agent.secrets import (
    ENVIRONMENT_VARIABLES,
    EnvironmentSecretProvider,
    LazySecretProvider,
    SecretName,
    SecretReadError,
    SecretSource,
    SecretValue,
)


def _decision_input() -> OpaDecisionInput:
    return OpaDecisionInput(
        subject_id="u1",
        tenant_id="t1",
        roles=("tenant_admin",),
        asset_ids=(),
        site_ids=(),
        action="work_order.close",
        resource_tenant_id="t1",
        resource_id=None,
        resource_asset_id=None,
        resource_site_id=None,
    )


class _FakeResponse:
    def raise_for_status(self) -> None:
        return None

    def json(self) -> dict:
        return {
            "result": {
                "allow": True,
                "reason": "test_allow",
                "obligations": [],
                "policy_version": "t",
            }
        }


def test_client_sends_bearer_token_header(monkeypatch: pytest.MonkeyPatch) -> None:
    captured: dict[str, object] = {}

    def fake_post(url, *, json, headers=None, timeout=None):  # noqa: ANN001
        captured["url"] = url
        captured["headers"] = headers
        return _FakeResponse()

    monkeypatch.setattr(httpx, "post", fake_post)
    client = OpaPolicyClient(
        "http://opa:8181/v1/data/industrial_ops/authz/result", token="probe-secret"
    )
    decision = client.decide(_decision_input())
    assert decision.allowed is True
    assert captured["headers"] == {"Authorization": "Bearer probe-secret"}


def test_client_omits_header_without_token(monkeypatch: pytest.MonkeyPatch) -> None:
    captured: dict[str, object] = {}

    def fake_post(url, *, json, headers=None, timeout=None):  # noqa: ANN001
        captured["headers"] = headers
        return _FakeResponse()

    monkeypatch.setattr(httpx, "post", fake_post)
    client = OpaPolicyClient("http://opa:8181/v1/data/industrial_ops/authz/result")
    client.decide(_decision_input())
    assert captured["headers"] is None


def test_client_maps_http_error_to_opa_unavailable(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    def fake_post(url, *, json, headers=None, timeout=None):  # noqa: ANN001
        raise httpx.ConnectError("refused")

    monkeypatch.setattr(httpx, "post", fake_post)
    client = OpaPolicyClient(
        "http://opa:8181/v1/data/industrial_ops/authz/result", token="probe-secret"
    )
    with pytest.raises(OpaUnavailable):
        client.decide(_decision_input())


def test_opa_token_env_mapping_matches_server_side_name() -> None:
    # opa 容器与应用容器共用同一环境变量名（compose 插值 OPA_TOKEN）
    assert ENVIRONMENT_VARIABLES[SecretName.OPA_TOKEN] == "OPA_TOKEN"


def test_lazy_provider_falls_back_to_env_only_for_exempt_names() -> None:
    class AlwaysMissing:
        def get(self, name: SecretName) -> SecretValue:
            raise SecretReadError(name, SecretSource.VAULT, "missing")

        def refresh(self) -> None:
            return None

    provider = LazySecretProvider(
        AlwaysMissing(), EnvironmentSecretProvider({"OPA_TOKEN": "from-env"})
    )
    # 豁免键：主查失败 → 环境变量兜底
    assert provider.get(SecretName.OPA_TOKEN).reveal() == "from-env"
    # 非豁免键：维持主查行为（缺失即抛错，不静默降级）
    with pytest.raises(SecretReadError):
        provider.get(SecretName.DATABASE_URL)
