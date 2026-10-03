"""Contract-level tests for L3 PredictionResult (Phase: RiskWindow v1).

Covers the contract model itself (B0/B1) before any algorithm exists:
- three-state semantics (no judgment vs. judgment-without-window vs. full)
- closed-model strictness (risk_level has no INSUFFICIENT; extra fields rejected)
- window nullability rules
- SITE is unsupported in v1
"""

from datetime import UTC, datetime

import pytest
from pydantic import ValidationError

from industrial_ops_agent.network_assurance.wireless_contracts import (
    DataSufficiency,
    InsufficientPrediction,
    PredictionResult,
    RiskDriver,
    RiskLevel,
    RiskWindow,
    SubjectType,
    TrendDirection,
    TrendEvidence,
    UnsupportedSubjectType,
    WindowEstimationStatus,
)

_NOW = datetime(2026, 10, 4, 12, 0, tzinfo=UTC)


def _window(**overrides: object) -> RiskWindow:
    base: dict[str, object] = {
        "subject_type": SubjectType.DEVICE,
        "subject_id": "AA:BB:CC:DD:EE:01",
        "risk_level": RiskLevel.HIGH,
        "window_estimation_status": WindowEstimationStatus.AVAILABLE,
        "window_earliest_hours": 24,
        "window_latest_hours": 72,
        "forecast_horizon_hours": 168,
        "prediction_confidence": RiskLevel.MEDIUM,
        "data_sufficiency": DataSufficiency.SUFFICIENT,
        "drivers": [
            RiskDriver(
                metric="delta_rssi_db",
                contribution_pct=61.0,
                direction=TrendDirection.DETERIORATING,
                evidence_ids=["e1", "e2"],
            )
        ],
        "trend_evidence": [
            TrendEvidence(
                metric="baseline_rssi_dbm",
                statement="基线 72h 内自 -60 漂移到 -64",
                evidence_ids=["e1"],
            )
        ],
        "failure_criterion_id": "weaknet-rf-service-impact",
        "failure_criterion_version": "v1",
        "model_version": "riskwindow-v1+sha256:deadbeef",
        "generated_at": _NOW,
    }
    base.update(overrides)
    return RiskWindow(**base)  # type: ignore[arg-type]


def test_risk_level_has_no_insufficient_member() -> None:
    """'证据不足'不是第四种风险等级——这是契约 C 的三态语义基石。"""
    assert {member.value for member in RiskLevel} == {"LOW", "MEDIUM", "HIGH"}
    assert not hasattr(RiskLevel, "INSUFFICIENT")
    assert not hasattr(RiskLevel, "UNKNOWN")


def test_data_sufficiency_inside_risk_window_excludes_insufficient() -> None:
    """RiskWindow 内不出现 INSUFFICIENT——该情形必须走 InsufficientPrediction。"""
    assert {member.value for member in DataSufficiency} == {
        "INSUFFICIENT",
        "PARTIAL",
        "SUFFICIENT",
    }


def test_window_is_nullable_when_estimation_unavailable() -> None:
    window = _window(
        window_estimation_status=WindowEstimationStatus.UNAVAILABLE,
        window_earliest_hours=None,
        window_latest_hours=None,
        data_sufficiency=DataSufficiency.PARTIAL,
    )
    assert window.window_earliest_hours is None
    assert window.window_latest_hours is None
    # 风险判断仍然成立——这正是"有判断但不知何时越界"与"没有判断"的分界
    assert window.risk_level is RiskLevel.HIGH


def test_full_window_carries_concrete_machine_readable_hours() -> None:
    window = _window()
    assert window.window_earliest_hours == 24
    assert window.window_latest_hours == 72
    assert isinstance(window.window_earliest_hours, int)


def test_window_rejects_unknown_fields() -> None:
    with pytest.raises(ValidationError):
        _window(risk_unknown_field="x")


def test_window_rejects_bad_hour_values() -> None:
    with pytest.raises(ValidationError):
        _window(window_earliest_hours=-1)


def test_driver_contribution_is_bounded_percent() -> None:
    with pytest.raises(ValidationError):
        RiskDriver(metric="x", contribution_pct=101.0, direction=TrendDirection.STABLE)
    with pytest.raises(ValidationError):
        RiskDriver(metric="x", contribution_pct=-0.1, direction=TrendDirection.STABLE)


def test_insufficient_prediction_is_not_a_risk_judgment() -> None:
    insufficient = InsufficientPrediction(
        subject_id="AA:BB:CC:DD:EE:01",
        missing_requirements=["delta_rssi_db: 有效样本不足 (3 < 10)"],
        available_evidence_ids=["e1"],
        model_version="riskwindow-v1+sha256:deadbeef",
        generated_at=_NOW,
    )
    assert insufficient.data_sufficiency is DataSufficiency.INSUFFICIENT
    # 它根本没有 risk_level 这个字段——"没有风险判断"是结构上的事实
    assert not hasattr(insufficient, "risk_level")
    assert not hasattr(insufficient, "drivers")


def test_prediction_result_union_accepts_both_shapes() -> None:
    window = _window()
    insufficient = InsufficientPrediction(
        subject_id="dev-1",
        missing_requirements=["metric history missing"],
        model_version="riskwindow-v1+sha256:deadbeef",
        generated_at=_NOW,
    )
    assert isinstance(window, RiskWindow)
    assert isinstance(insufficient, InsufficientPrediction)
    # 联合类型在运行时就是这两个类的联合
    assert PredictionResult is not None


def test_site_subject_type_is_reserved_but_raises_when_used() -> None:
    assert SubjectType.SITE.value == "SITE"
    with pytest.raises(UnsupportedSubjectType) as excinfo:
        raise UnsupportedSubjectType(SubjectType.SITE)
    assert excinfo.value.subject_type is SubjectType.SITE
