"""RiskPredictionService — 单设备 L3 预测的组装层（路线图 ③ 后端）。

职责：把契约 C 的 `PredictionResult` 生产管线接到真实数据源——
只做"取数 + 调用"，不含任何算法（算法在 risk_window，取数在 metric_history）。

    baseline row 定位 → MetricHistoryProvider(三路历史)
        → risk_window.predict() → PredictionResult
"""

from __future__ import annotations

from collections.abc import Callable
from datetime import UTC, datetime

from sqlalchemy import select

from industrial_ops_agent.network_assurance.metric_history import MetricHistoryProvider
from industrial_ops_agent.network_assurance.risk_window import (
    RiskWindowConfig,
    model_version_for,
    predict,
)
from industrial_ops_agent.network_assurance.wireless_contracts import (
    InsufficientPrediction,
    PredictionResult,
    SubjectType,
)
from industrial_ops_agent.persistence.database import Database
from industrial_ops_agent.persistence.models import NetworkDeviceBaselineRecord
from industrial_ops_agent.persistence.tenant import TenantContext

_HOUR_MS = 3_600_000


class RiskPredictionService:
    def __init__(self, database: Database) -> None:
        self._database = database
        self._provider = MetricHistoryProvider(database)

    def predict_device(
        self,
        context: TenantContext,
        asset_id: str,
        device_address: str,
        *,
        horizon_hours: int = 168,
        now_ms: int | None = None,
    ) -> PredictionResult:
        """对一台外围无线设备给出预测结果（三态之一）。

        - ``now_ms`` 可注入（测试确定性；生产缺省取当前墙钟）；
        - 无基线行 → ``InsufficientPrediction``（如实列出缺失，不编造）；
        - SITE 主体不在本服务范围内（v1 显式 UnsupportedSubjectType）。
        """
        now = now_ms if now_ms is not None else int(datetime.now(UTC).timestamp() * 1_000)
        config = RiskWindowConfig(horizon_hours=horizon_hours)
        start_ms = now - horizon_hours * _HOUR_MS
        clock: Callable[[], datetime]
        if now_ms is None:
            clock = lambda: datetime.now(UTC)  # noqa: E731
        else:
            clock = lambda: datetime.fromtimestamp(now / 1_000, tz=UTC)  # noqa: E731

        # 同设备存在多行（不同 site/gateway 视图）时取最近更新的基线身份
        with self._database.transaction(context) as session:
            row = session.scalars(
                select(NetworkDeviceBaselineRecord)
                .where(
                    NetworkDeviceBaselineRecord.tenant_id == context.tenant_id,
                    NetworkDeviceBaselineRecord.asset_id == asset_id,
                    NetworkDeviceBaselineRecord.device_address == device_address,
                )
                .order_by(NetworkDeviceBaselineRecord.updated_at.desc())
                .limit(1)
            ).first()

        if row is None:
            # 三态之一：没有风险判断产生（不是"风险未知"）
            return InsufficientPrediction(
                subject_id=device_address,
                missing_requirements=[
                    f"network_device_baselines 无 {device_address} 的基线行"
                ],
                available_evidence_ids=[],
                model_version=model_version_for(config),
                generated_at=clock(),
            )

        histories = {
            "baseline_rssi_dbm": self._provider.load_baseline_trajectory(
                context, row.baseline_id, start_ms, now
            ),
            "packet_loss_rate": self._provider.load_snapshot_evidence(
                context, asset_id, "wifi_loss_rate", start_ms, now, unit_scale=0.01
            ),
            "median_jitter_ms": self._provider.load_snapshot_evidence(
                context, asset_id, "median_jitter_ms", start_ms, now
            ),
        }
        return predict(
            SubjectType.DEVICE,
            device_address,
            histories,
            clock=clock,
            config=config,
        )
