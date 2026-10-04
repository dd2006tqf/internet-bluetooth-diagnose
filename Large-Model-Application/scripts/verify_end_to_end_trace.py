#!/usr/bin/env python3
"""verify_end_to_end_trace —— ⑥ 审计链只读报告器（read-only）。

两种模式：

* ``--trace-report <file.json>``：**完整验证模式**。读场景 runner 写出的
  TraceReport（含运行时捕获的 prediction snapshot 与被拒操作记录），
  逐项核对 edge/operation 断言并打印审计树。可输出 ``TRACE OK``。

* ``--incident-id <id>``：**数据库审计模式**。只重建 DB 持久化的链
  （proposal/decision/approval/pending_action/outcome），Prediction 与
  "尝试过但被拒"的操作无法事后恢复——因此结果上限是 ``TRACE PARTIAL``，
  绝不输出完整 ``TRACE OK``。

本工具不写数据库、不重跑 Prediction、不修改任何状态。
"""

from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass

from industrial_ops_agent.network_assurance.e2e_trace import (
    TraceEdgeCheck,
    TraceNode,
    TraceReport,
    render_tree,
    verify_trace,
)


@dataclass
class _DbViews:
    proposals: list[dict]
    decisions: list[dict]
    approvals: list[dict]
    pending: list[dict]
    outcomes: list[dict]


def _load_db_views(conn, incident_id: str) -> _DbViews:
    """只读查询：把 incident 相关行拽出来（reporter 的 DB 上限）。"""
    cur = conn.cursor()
    cur.execute(
        "SELECT council_id FROM network_councils WHERE incident_id = ?",
        (incident_id,),
    )
    councils = [r[0] for r in cur.fetchall()]

    def q(sql, *args):
        cur.execute(sql, args)
        cols = [d[0] for d in cur.description]
        return [dict(zip(cols, r, strict=False)) for r in cur.fetchall()]

    if councils:
        placeholder = ",".join("?" * len(councils))
        proposals = q(
            f"SELECT * FROM network_council_proposals WHERE council_id IN ({placeholder})",
            *councils,
        )
        pids = [p["proposal_id"] for p in proposals]
        if pids:
            ph = ",".join("?" * len(pids))
            decisions = q(
                f"SELECT * FROM network_action_decisions WHERE proposal_id IN ({ph})",
                *pids,
            )
            approvals = q(
                f"SELECT * FROM network_action_approvals WHERE proposal_id IN ({ph})",
                *pids,
            )
        else:
            decisions, approvals = [], []
    else:
        proposals, decisions, approvals = [], [], []

    pending = q(
        "SELECT * FROM network_pending_actions WHERE approval_id IN"
        " (SELECT approval_id FROM network_action_approvals WHERE council_id IN"
        f" ({','.join('?' * len(councils))}))",
        *councils,
    ) if councils else []
    outcomes = q(
        "SELECT * FROM network_action_outcomes WHERE pending_action_id IN"
        f" ({','.join('?' * len([p['action_id'] for p in pending]))})",
        *[p["action_id"] for p in pending],
    ) if pending else []

    return _DbViews(
        proposals=proposals,
        decisions=decisions,
        approvals=approvals,
        pending=pending,
        outcomes=outcomes,
    )


def _report_from_db(incident_id: str, views: _DbViews) -> TraceReport:
    """DB 审计模式的重建：只放持久化节点，不虚构未持久化的部分。"""
    nodes: list[TraceNode] = [
        TraceNode(kind="INCIDENT", id=incident_id, status="persisted")
    ]
    edges: list[TraceEdgeCheck] = []
    # Council
    council_ids = {p["council_id"] for p in views.proposals}
    for cid in sorted(council_ids):
        nodes.append(TraceNode(kind="COUNCIL", id=cid, status="persisted"))
    # Proposals + Decisions
    for p in sorted(views.proposals, key=lambda r: r["proposal_index"]):
        nodes.append(
            TraceNode(
                kind="PROPOSAL",
                id=p["proposal_id"],
                digest_or_version=p["proposal_digest"][:24],
                status="persisted",
            )
        )
        dec = next(
            (d for d in views.decisions if d["proposal_id"] == p["proposal_id"]),
            None,
        )
        if dec:
            nodes.append(
                TraceNode(
                    kind="ACTION_DECISION",
                    id=dec["decision_id"],
                    status="ALLOWED" if dec["allowed"] else "BLOCKED",
                    digest_or_version=dec["decision_digest"][:24],
                    extra={
                        "risk": dec["risk"],
                        "execution_mode": dec["execution_mode"],
                        "block_reason": dec["block_reason"],
                    },
                )
            )
            edges.append(
                TraceEdgeCheck("PROPOSAL", "ACTION_DECISION", "PRESENT", "PRESENT")
            )
            appr = next(
                (a for a in views.approvals if a["proposal_id"] == p["proposal_id"]),
                None,
            )
            edges.append(
                TraceEdgeCheck(
                    "ACTION_DECISION",
                    "APPROVAL",
                    "PRESENT" if dec["allowed"] else "ABSENT",
                    "PRESENT" if appr else "ABSENT",
                    reason="blocked_proposal_has_no_approval",
                )
            )
            if appr:
                nodes.append(
                    TraceNode(
                        kind="APPROVAL",
                        id=appr["approval_id"],
                        status=appr["status"],
                        extra={"execution_status": appr["execution_status"]},
                    )
                )
                pa = next(
                    (x for x in views.pending if x.get("approval_id") == appr["approval_id"]),
                    None,
                )
                if pa:
                    nodes.append(
                        TraceNode(
                            kind="PENDING_ACTION",
                            id=pa["action_id"],
                            status=pa["status"],
                            extra={
                                "config": f"{pa['config_key']}={pa['config_value']}",
                                "approved_by": pa["approved_by"],
                            },
                        )
                    )
                    oc = next(
                        (
                            o
                            for o in views.outcomes
                            if o["pending_action_id"] == pa["action_id"]
                        ),
                        None,
                    )
                    edges.append(
                        TraceEdgeCheck(
                            "PENDING_ACTION",
                            "ACTION_OUTCOME",
                            "PRESENT"
                            if pa["status"] in ("APPLIED", "REJECTED", "ROLLBACK")
                            else "ABSENT",
                            "PRESENT" if oc else "ABSENT",
                            reason="terminal_pending_must_have_outcome",
                        )
                    )
                    if oc:
                        nodes.append(
                            TraceNode(
                                kind="ACTION_OUTCOME",
                                id=oc["outcome_id"],
                                status=oc["status"],
                                ts=oc["completed_at"],
                                digest_or_version=oc["action_payload_digest"][:24],
                            )
                        )
                elif appr["manual_execution_required"]:
                    nodes.append(
                        TraceNode(
                            kind="MANUAL_RUNBOOK",
                            id=appr["approval_id"],
                            status="NOT_EXECUTED",
                        )
                    )

    return TraceReport(
        scenario="REMOTE_HAPPY",  # 占位：DB 模式不区分场景
        incident_id=incident_id,
        nodes=nodes,
        edge_checks=edges,
        operation_checks=[],  # DB 模式无法重建"被拒的操作"——如实留空
        prediction_snapshots=[],  # Prediction 不落库——如实标注
        completed_at=None,
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    src = parser.add_mutually_exclusive_group(required=True)
    src.add_argument("--trace-report", help="runner 写出的 TraceReport JSON")
    src.add_argument("--incident-id", help="从 DB 重建持久化链（结果上限 PARTIAL）")
    parser.add_argument(
        "--db-url",
        default="sqlite:///data/app.db",
        help="SQLAlchemy URL（默认 sqlite:///data/app.db）",
    )
    parser.add_argument("--json", action="store_true", help="输出 TraceReport JSON")
    args = parser.parse_args()

    if args.trace_report:
        with open(args.trace_report, encoding="utf-8") as fh:
            report = TraceReport.from_json(fh.read())
        verdict = verify_trace(report)
        out = render_tree(report, verdict)
        print(out)
        if args.json:
            print("\n---\n" + report.to_json())
        return 0 if verdict.result == "OK" else 1

    # --incident-id：DB 审计模式，上限 PARTIAL
    import os
    import sqlite3

    from sqlalchemy.engine import make_url

    try:
        url = make_url(args.db_url)
    except Exception as exc:  # noqa: BLE001 - CLI 输入
        print(f"invalid --db-url: {exc}", file=sys.stderr)
        return 2
    if not url.drivername.startswith("sqlite"):
        print("reporter v1 只支持 sqlite URL", file=sys.stderr)
        return 2
    # make_url 已按 SQLAlchemy 约定解析：sqlite:///x = 相对 x，sqlite:////x = 绝对 /x
    path = url.database or ""
    if path != ":memory:" and not os.path.exists(path):
        print(f"database not found: {path}", file=sys.stderr)
        return 2
    conn = sqlite3.connect(path)
    try:
        views = _load_db_views(conn, args.incident_id)
    finally:
        conn.close()

    report = _report_from_db(args.incident_id, views)
    verdict = verify_trace(report)
    # DB 模式永远 PARTIAL（Prediction/被拒操作不可恢复）
    if verdict.result == "OK":
        verdict = type(verdict)(
            result="PARTIAL",
            first_break=None,
            details=verdict.details + ["DB-audit mode: prediction/attempted-ops not persisted"],
        )
    print(render_tree(report, verdict))
    if args.json:
        print("\n---\n" + report.to_json())
    return 0 if verdict.result in ("OK", "PARTIAL") else 1


if __name__ == "__main__":
    raise SystemExit(main())
