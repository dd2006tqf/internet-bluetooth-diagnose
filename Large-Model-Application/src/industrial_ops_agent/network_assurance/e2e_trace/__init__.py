"""e2e_trace —— 路线图 ⑥ 审计链验证的辅助契约与 verifier。

非产品领域层：不持久化、不参与运行时请求路径；由测试与
``scripts/verify_end_to_end_trace.py`` 复用。
"""

from .trace_contract import (
    TRACE_CONTRACT_VERSION,
    NodeKind,
    TraceEdgeCheck,
    TraceNode,
    TraceOperationCheck,
    TraceReport,
    edge,
    node,
)
from .trace_verifier import TraceVerdict, render_tree, verify_trace

__all__ = [
    "TRACE_CONTRACT_VERSION",
    "NodeKind",
    "TraceEdgeCheck",
    "TraceNode",
    "TraceOperationCheck",
    "TraceReport",
    "TraceVerdict",
    "edge",
    "node",
    "render_tree",
    "verify_trace",
]
