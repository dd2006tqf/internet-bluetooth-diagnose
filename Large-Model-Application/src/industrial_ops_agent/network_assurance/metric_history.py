"""MetricHistoryProvider — L3 的统一历史读取层（B3）。

职责边界：SQL、去重、种子补齐、迟到/间隙语义**只在这里**处理，
`risk_window.py` 的预测器只接收排好序的 `MetricSample` 序列。

两类历史源：

1. **baseline change-log**（`network_device_baseline_history`，B3a）：
   change-log 的窗口起点状态会丢（"Day2 起点仍是 -60"不在窗口行里），
   因此 load 语义是::

       load_history(start, end)
         = latest sample <= start      # seed / carry-forward（受陈旧期约束）
         + all changes in (start, end]

   预 migation 存量设备没有历史行时，退化用当前态行的 updated_at 作种子。
   成熟度（mature）：baseline_sample_count >= min_baseline_samples 且 state == STABLE
   —— 只有成熟基线才有资格充当预测锚点（契约 C 纪律 6）。

2. **snapshot evidence**（`network_snapshots.experience_json`）：
   递归抽取 SLE evidence 条目（`{"metric": name, "value": v}`），
   按 unit_scale 换算单位（如板端 % → ratio），evidence_id = snapshot_id。
"""

from __future__ import annotations

from datetime import UTC, datetime
from typing import Any

from sqlalchemy import select

from industrial_ops_agent.domain import as_utc
from industrial_ops_agent.network_assurance.risk_window import MetricSample
from industrial_ops_agent.persistence.database import Database
from industrial_ops_agent.persistence.models import (
    NetworkDeviceBaselineHistoryRecord,
    NetworkDeviceBaselineRecord,
    NetworkSnapshotRecord,
)
from industrial_ops_agent.persistence.tenant import TenantContext

_MILLIS = 1_000


def _ms_to_dt(ms: int) -> datetime:
    return datetime.fromtimestamp(ms / _MILLIS, tz=UTC)


def _dt_to_ms(dt: datetime) -> int:
    return int(as_utc(dt).timestamp() * _MILLIS)


class MetricHistoryProvider:
    def __init__(
        self,
        database: Database,
        *,
        min_baseline_samples: int = 10,
        seed_staleness_hours: int = 336,
    ) -> None:
        self._database = database
        self._min_baseline_samples = min_baseline_samples
        self._seed_staleness_hours = seed_staleness_hours

    # ------------------------------------------------------------------
    # baseline change-log
    # ------------------------------------------------------------------

    def load_baseline_trajectory(
        self,
        context: TenantContext,
        baseline_id: str,
        start_ms: int,
        end_ms: int,
    ) -> list[MetricSample]:
        """seed/carry-forward + 窗口内全部变更（时间升序）。"""
        staleness_floor_ms = (
            start_ms - self._seed_staleness_hours * 3_600 * _MILLIS
        )
        seed: MetricSample | None = None

        with self._database.transaction(context) as session:
            seed_row = session.scalars(
                select(NetworkDeviceBaselineHistoryRecord)
                .where(
                    NetworkDeviceBaselineHistoryRecord.tenant_id == context.tenant_id,
                    NetworkDeviceBaselineHistoryRecord.baseline_id == baseline_id,
                    NetworkDeviceBaselineHistoryRecord.observed_at_ms <= start_ms,
                    NetworkDeviceBaselineHistoryRecord.observed_at_ms >= staleness_floor_ms,
                )
                .order_by(NetworkDeviceBaselineHistoryRecord.observed_at_ms.desc())
                .limit(1)
            ).first()
            if seed_row is not None and seed_row.baseline_rssi_dbm is not None:
                seed = self._to_sample(
                    seed_row.history_id,
                    seed_row.observed_at_ms,
                    seed_row.baseline_rssi_dbm,
                    seed_row.baseline_sample_count,
                    seed_row.state,
                )
            else:
                # 存量设备（历史表迁移前就有当前态行）：用当前行补一个种子
                current = session.get(NetworkDeviceBaselineRecord, baseline_id)
                if current is not None:
                    current_ts = _dt_to_ms(current.updated_at)
                    if staleness_floor_ms <= current_ts <= start_ms and (
                        current.baseline_rssi_dbm is not None
                    ):
                        seed = self._to_sample(
                            f"current:{baseline_id}",
                            current_ts,
                            current.baseline_rssi_dbm,
                            current.baseline_sample_count,
                            current.state,
                        )

            change_rows = session.scalars(
                select(NetworkDeviceBaselineHistoryRecord)
                .where(
                    NetworkDeviceBaselineHistoryRecord.tenant_id == context.tenant_id,
                    NetworkDeviceBaselineHistoryRecord.baseline_id == baseline_id,
                    NetworkDeviceBaselineHistoryRecord.observed_at_ms > start_ms,
                    NetworkDeviceBaselineHistoryRecord.observed_at_ms <= end_ms,
                )
                .order_by(NetworkDeviceBaselineHistoryRecord.observed_at_ms.asc())
            ).all()

        samples: list[MetricSample] = []
        if seed is not None:
            samples.append(seed)
        for row in change_rows:
            if row.baseline_rssi_dbm is None:
                continue  # 未采集不进序列（NULL ≠ 0）
            samples.append(
                self._to_sample(
                    row.history_id,
                    row.observed_at_ms,
                    row.baseline_rssi_dbm,
                    row.baseline_sample_count,
                    row.state,
                )
            )
        return samples

    def _to_sample(
        self,
        evidence_id: str,
        observed_at_ms: int,
        value: int,
        sample_count: int,
        state: str,
    ) -> MetricSample:
        return MetricSample(
            ts_ms=observed_at_ms,
            value=float(value),
            evidence_id=evidence_id,
            mature=sample_count >= self._min_baseline_samples and state == "STABLE",
        )

    # ------------------------------------------------------------------
    # snapshot evidence
    # ------------------------------------------------------------------

    def load_snapshot_evidence(
        self,
        context: TenantContext,
        asset_id: str,
        evidence_metric: str,
        start_ms: int,
        end_ms: int,
        *,
        unit_scale: float = 1.0,
        limit: int = 5000,
    ) -> list[MetricSample]:
        """从 experience_json 递归抽取指定 evidence metric（时间升序）。"""
        start_dt = _ms_to_dt(start_ms)
        end_dt = _ms_to_dt(end_ms)
        with self._database.transaction(context) as session:
            rows = session.scalars(
                select(NetworkSnapshotRecord)
                .where(
                    NetworkSnapshotRecord.tenant_id == context.tenant_id,
                    NetworkSnapshotRecord.asset_id == asset_id,
                    NetworkSnapshotRecord.captured_at >= start_dt,
                    NetworkSnapshotRecord.captured_at <= end_dt,
                )
                .order_by(NetworkSnapshotRecord.captured_at.asc())
                .limit(limit)
            ).all()

        samples: list[MetricSample] = []
        for row in rows:
            value = _find_evidence_value(row.experience_json, evidence_metric)
            if value is None:
                continue  # 该快照没有此指标 → 缺失，不当 0
            samples.append(
                MetricSample(
                    ts_ms=_dt_to_ms(row.captured_at),
                    value=float(value) * unit_scale,
                    evidence_id=row.snapshot_id,
                )
            )
        return samples


def _find_evidence_value(node: Any, metric: str) -> float | None:
    """在 experience_json 中递归找第一个 metric 匹配的 evidence 条目。

    返回 None 表示该快照未上报此指标（缺失 ≠ 0，契约纪律 4）。
    """
    if isinstance(node, dict):
        for key, value in node.items():
            if key == "evidence" and isinstance(value, list):
                for entry in value:
                    if isinstance(entry, dict) and entry.get("metric") == metric:
                        raw = entry.get("value")
                        if isinstance(raw, (int, float)):
                            return float(raw)
            else:
                found = _find_evidence_value(value, metric)
                if found is not None:
                    return found
    elif isinstance(node, list):
        for item in node:
            found = _find_evidence_value(item, metric)
            if found is not None:
                return found
    return None
