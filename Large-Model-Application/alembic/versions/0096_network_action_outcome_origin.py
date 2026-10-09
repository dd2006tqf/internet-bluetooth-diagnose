"""Add network_action_outcomes.execution_origin —— 执行来源事实（L1 outcome 自解释）。

Revision ID: 0096_network_action_outcome_origin
Revises: 0095_network_gateway_catalog_version
Create Date: 2026-10-09

`network_action_outcomes` 已记录 proposal_id / approval_id，但二者可空——
于是"这条执行为什么没有 Council provenance"每次查询都要靠 NULL 反推
（是人工直发？还是数据缺失？）。本列把**来源**显式物化为事实。

取值闭集（当前由 `queue_action` 的两个调用点穷尽）：
  - COUNCIL_APPROVED  —— 经 Network Operations Council → Policy → Approval 链
  - MANUAL_OPERATION  —— 运维经 direct queue 直接入队

**注意语义**：它只是"来源事实"，**不是 Policy 判断**。COUNCIL_APPROVED
不表示该动作安全，只表示它走的是审批链；MANUAL_OPERATION 也不表示不安全。

既有行按 proposal_id 是否存在回填——这正是来源的判据本身
（审批链必然写入 proposal_id/approval_id，direct queue 必然不带）。
"""

from __future__ import annotations

from collections.abc import Sequence

import sqlalchemy as sa
from alembic import op

revision: str = "0096_network_action_outcome_origin"
down_revision: str | None = "0095_network_gateway_catalog_version"
branch_labels: str | Sequence[str] | None = None
depends_on: str | Sequence[str] | None = None


def upgrade() -> None:
    # 1) 先加可空列
    op.add_column(
        "network_action_outcomes",
        sa.Column("execution_origin", sa.String(32), nullable=True),
    )

    # 2) 回填：审批链带 proposal_id，direct queue 不带
    op.execute(
        "UPDATE network_action_outcomes "
        "SET execution_origin = CASE WHEN proposal_id IS NULL "
        "THEN 'MANUAL_OPERATION' ELSE 'COUNCIL_APPROVED' END"
    )

    # 3) 收紧为 NOT NULL + 闭集约束
    op.alter_column("network_action_outcomes", "execution_origin", nullable=False)
    op.create_check_constraint(
        "ck_network_action_outcome_origin",
        "network_action_outcomes",
        "execution_origin IN ('COUNCIL_APPROVED', 'MANUAL_OPERATION')",
    )


def downgrade() -> None:
    op.drop_constraint(
        "ck_network_action_outcome_origin",
        "network_action_outcomes",
        type_="check",
    )
    op.drop_column("network_action_outcomes", "execution_origin")
