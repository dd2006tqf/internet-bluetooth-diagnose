"""Extend network_action_outcomes.execution_origin —— 允许 LOCAL_OPERATION。

Revision ID: 0097_network_action_outcome_local_origin
Revises: 0096_network_action_outcome_origin
Create Date: 2026-10-09

0096 把执行来源物化为闭集 {COUNCIL_APPROVED, MANUAL_OPERATION}，但那个闭集
只覆盖**经云端队列入队**的两条路径。板端 D-Bus 的本地调参
（weaknet-cli set → SetMonitorParam）从不进 network_pending_actions：云端
此前只能看到 config_generation 跳变，无从判断改了哪个 key，于是一次真实的
人工作业在 L1 事实域里完全不留痕。

本迁移放开闭集，纳入 LOCAL_OPERATION = "板端本机发起、未经云端队列"。
回执自带 config_key/config_value/generation 才能落成一行——见
`NetworkAssuranceService.record_action_results` 的本地分支。

语义同 0096：它只是来源事实，不是 Policy 判断——LOCAL_OPERATION 不表示该
改动危险，只表示它没走审批链（这本身正是运维需要看见的事实）。

downgrade 会按约束语义校验既有行：若表中已有 LOCAL_OPERATION 行，回滚会
**失败**。这是刻意的——append-only 事实表不得为了让迁移可回滚而删除事实。
"""

from __future__ import annotations

from collections.abc import Sequence

from alembic import op

revision: str = "0097_network_action_outcome_local_origin"
down_revision: str | None = "0096_network_action_outcome_origin"
branch_labels: str | Sequence[str] | None = None
depends_on: str | Sequence[str] | None = None

_ORIGIN_VALUES = "'COUNCIL_APPROVED', 'MANUAL_OPERATION', 'LOCAL_OPERATION'"


def upgrade() -> None:
    op.drop_constraint(
        "ck_network_action_outcome_origin",
        "network_action_outcomes",
        type_="check",
    )
    op.create_check_constraint(
        "ck_network_action_outcome_origin",
        "network_action_outcomes",
        f"execution_origin IN ({_ORIGIN_VALUES})",
    )


def downgrade() -> None:
    op.drop_constraint(
        "ck_network_action_outcome_origin",
        "network_action_outcomes",
        type_="check",
    )
    op.create_check_constraint(
        "ck_network_action_outcome_origin",
        "network_action_outcomes",
        "execution_origin IN ('COUNCIL_APPROVED', 'MANUAL_OPERATION')",
    )
