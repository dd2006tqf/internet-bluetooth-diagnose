"""Governed telemetry and predictive-maintenance application boundary."""

from typing import Any

# 延迟导入以避免无 pandas 环境在 import 本包时直接崩溃
def __getattr__(name: str) -> Any:
    if name == "PredictiveMaintenanceService":
        from industrial_ops_agent.predictive_maintenance.service import PredictiveMaintenanceService
        return PredictiveMaintenanceService
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")

__all__ = ["PredictiveMaintenanceService"]

