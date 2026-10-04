"use client";

/**
 * CouncilPanel —— ⑤ Network Operations Council 会商/审批面板。
 *
 * 呈现顺序（四权分立的人读化）：
 *   召集状态 → 三专家意见 → 提案表（Policy 裁决 + 审批状态）
 *   → 批准/拒绝（If-Match 乐观并发）→ REMOTE 执行 / MANUAL 运行手册
 *
 * 语义红线（与后端一致）：
 * - Policy 裁决（allowed/risk/block_reason）与人裁（APPROVED/REJECTED）分栏展示；
 * - MANUAL_RUNBOOK 永远 NOT_EXECUTED——UI 不得渲染成"已执行"；
 * - 执行载荷不来自表单——/execute 只传 proposal_id + Idempotency-Key。
 */

import {
  Alert,
  Badge,
  Button,
  Card,
  Descriptions,
  Input,
  Modal,
  Popover,
  Space,
  Table,
  Tag,
  Tooltip,
  Typography,
  message,
} from "antd";
import { useCallback, useEffect, useState } from "react";

import { apiClient } from "@/lib/api/network";
import type {
  CouncilProposalState,
  ManualRunbook,
  NetworkCouncilView,
} from "@/lib/api/network";

const { Text, Paragraph } = Typography;
const { TextArea } = Input;

function statusBadge(status: string) {
  switch (status) {
    case "REVIEW_PENDING":
      return <Badge status="processing" text="会商完成" />;
    case "FAILED":
      return <Badge status="error" text="会商失败" />;
    case "RUNNING":
      return <Badge status="processing" text="会商中" />;
    default:
      return <Badge status="default" text={status} />;
  }
}

function approvalTag(status: string) {
  switch (status) {
    case "PENDING":
      return <Tag color="gold">待审批</Tag>;
    case "APPROVED":
      return <Tag color="green">已批准</Tag>;
    case "REJECTED":
      return <Tag color="red">已拒绝</Tag>;
    case "EXPIRED":
      return <Tag color="default">已过期</Tag>;
    case "SUPERSEDED":
      return <Tag color="default">已被取代</Tag>;
    default:
      return <Tag>{status}</Tag>;
  }
}

function riskTag(risk: string) {
  const color = risk === "HIGH" ? "red" : risk === "MEDIUM" ? "orange" : "green";
  return <Tag color={color}>{risk}</Tag>;
}

function describeProposal(p: CouncilProposalState): string {
  const snap = p.proposal_snapshot;
  if (snap.kind === "CONFIG_CHANGE") {
    return `调参 ${snap.config_key} = ${snap.config_value}`;
  }
  return `动作 ${snap.action_id ?? "-"}`;
}

export function CouncilPanel({
  incidentId,
  onChanged,
}: {
  incidentId: string;
  onChanged?: () => void;
}) {
  const [council, setCouncil] = useState<NetworkCouncilView | null>(null);
  const [proposals, setProposals] = useState<CouncilProposalState[]>([]);
  const [notFound, setNotFound] = useState(false);
  const [loading, setLoading] = useState(false);
  const [convening, setConvening] = useState(false);
  const [decideTarget, setDecideTarget] = useState<{
    proposal: CouncilProposalState;
    decision: "APPROVED" | "REJECTED";
  } | null>(null);
  const [reason, setReason] = useState("");
  const [runbook, setRunbook] = useState<ManualRunbook | null>(null);
  const [busyId, setBusyId] = useState<string | null>(null);

  const reload = useCallback(() => {
    setLoading(true);
    apiClient
      .getIncidentCouncil(incidentId)
      .then((view) => {
        setCouncil(view);
        setNotFound(false);
        return apiClient
          .listCouncilProposals(incidentId)
          .then(setProposals)
          .catch(() => setProposals([]));
      })
      .catch(() => {
        setCouncil(null);
        setNotFound(true);
      })
      .finally(() => setLoading(false));
  }, [incidentId]);

  useEffect(() => {
    reload();
  }, [reload]);

  const convene = async (force: boolean) => {
    setConvening(true);
    try {
      await apiClient.conveneCouncil(incidentId, force);
      message.success(force ? "已重新召集会商（新 attempt）" : "会商完成");
      reload();
      onChanged?.();
    } catch (err) {
      message.error(`召集失败: ${(err as Error).message}`);
    } finally {
      setConvening(false);
    }
  };

  const submitDecision = async () => {
    if (!decideTarget) return;
    const { proposal, decision } = decideTarget;
    if (!proposal.approval) return;
    try {
      await apiClient.decideProposal(
        proposal.proposal_id,
        decision,
        reason,
        proposal.approval.version,
      );
      message.success(decision === "APPROVED" ? "已批准" : "已拒绝");
      setDecideTarget(null);
      setReason("");
      reload();
    } catch (err) {
      message.error(`提交失败: ${(err as Error).message}`);
    }
  };

  const execute = async (proposal: CouncilProposalState) => {
    setBusyId(proposal.proposal_id);
    try {
      const result = await apiClient.executeProposal(
        proposal.proposal_id,
        `web-${proposal.proposal_id}-${Date.now()}`,
      );
      if (result.idempotent_replay) {
        message.info("该提案已执行过（幂等重放）");
      } else {
        message.success(`已下发到板端队列（${result.queued_action_id}）`);
      }
      reload();
    } catch (err) {
      message.error(`执行失败: ${(err as Error).message}`);
    } finally {
      setBusyId(null);
    }
  };

  const showRunbook = async (proposal: CouncilProposalState) => {
    try {
      const rb = await apiClient.getProposalRunbook(proposal.proposal_id);
      setRunbook(rb);
    } catch (err) {
      message.error(`获取运行手册失败: ${(err as Error).message}`);
    }
  };

  if (notFound) {
    return (
      <Space orientation="vertical" size={8} style={{ width: "100%" }}>
        <Alert
          type="info"
          showIcon
          message="尚未召集运维会商"
          description="会商将基于当前事实、诊断与预测产出建议动作；建议须经人工审批后才可能下发。"
        />
        <Button type="primary" loading={convening} onClick={() => convene(false)}>
          召集运维会商
        </Button>
      </Space>
    );
  }

  if (loading && !council) {
    return <Text type="secondary">会商数据加载中…</Text>;
  }

  return (
    <Space orientation="vertical" size={12} style={{ width: "100%" }}>
      {/* 召集状态行 */}
      <Space wrap>
        <Text strong>运维会商</Text>
        {council && statusBadge(council.status)}
        {council && (
          <Text type="secondary" style={{ fontSize: 12 }}>
            attempt {council.version} ·{" "}
            {new Date(council.updated_at).toLocaleString()}
          </Text>
        )}
        {council?.failure_code && (
          <Tag color="red">{council.failure_code}</Tag>
        )}
        <Button size="small" loading={convening} onClick={() => convene(!council)}>
          {council ? "重新召集（force）" : "召集运维会商"}
        </Button>
      </Space>

      {/* 专家意见 */}
      {council?.expert_opinions && council.expert_opinions.length > 0 && (
        <Card size="small" type="inner" title="专家意见">
          <Space orientation="vertical" size={4} style={{ width: "100%" }}>
            {council.expert_opinions.map((op) => (
              <div key={op.role}>
                <Tag color="geekblue">{op.role}</Tag>
                <Text>{op.recommendation_direction}</Text>
                {op.observations && op.observations.length > 0 && (
                  <Popover
                    content={
                      <ul style={{ margin: 0, paddingLeft: 18, maxWidth: 420 }}>
                        {op.observations.map((o) => (
                          <li key={o}>{o}</li>
                        ))}
                      </ul>
                    }
                  >
                    <Text type="secondary" style={{ fontSize: 12, marginLeft: 8 }}>
                      {op.observations.length} 条观察
                    </Text>
                  </Popover>
                )}
              </div>
            ))}
          </Space>
        </Card>
      )}

      {/* 提案表 */}
      {proposals.length > 0 && (
        <Table
          rowKey="proposal_id"
          size="small"
          dataSource={proposals}
          pagination={false}
          columns={[
            {
              title: "提案",
              key: "proposal",
              render: (_, p) => (
                <Space orientation="vertical" size={2}>
                  <Text>{describeProposal(p)}</Text>
                  <Tooltip title={p.proposal_id}>
                    <Text type="secondary" style={{ fontSize: 11 }}>
                      {p.proposal_id.slice(0, 24)}…
                    </Text>
                  </Tooltip>
                </Space>
              ),
            },
            {
              title: "Policy 裁决",
              key: "decision",
              render: (_, p) =>
                p.decision ? (
                  <Space orientation="vertical" size={2}>
                    <Space size={4}>
                      {p.decision.allowed ? (
                        <Tag color="green">ALLOWED</Tag>
                      ) : (
                        <Tag color="red">BLOCKED</Tag>
                      )}
                      {riskTag(p.decision.risk)}
                      <Tag>
                        {p.decision.execution_mode === "REMOTE_PENDING_ACTION"
                          ? "远程调参"
                          : "运行手册"}
                      </Tag>
                    </Space>
                    {p.decision.block_reason && (
                      <Text type="danger" style={{ fontSize: 11 }}>
                        {p.decision.block_reason}
                      </Text>
                    )}
                  </Space>
                ) : (
                  <Text type="secondary">—</Text>
                ),
            },
            {
              title: "审批",
              key: "approval",
              render: (_, p) =>
                p.approval ? (
                  <Space orientation="vertical" size={2}>
                    {approvalTag(p.approval.status)}
                    <Text type="secondary" style={{ fontSize: 11 }}>
                      {p.approval.status === "APPROVED"
                        ? `执行状态：${p.approval.execution_status}`
                        : `截止 ${new Date(p.approval.expires_at).toLocaleString()}`}
                    </Text>
                  </Space>
                ) : (
                  <Text type="secondary">无审批对象</Text>
                ),
            },
            {
              title: "操作",
              key: "ops",
              width: 260,
              render: (_, p) => {
                const a = p.approval;
                if (!a) return null;
                if (a.status === "PENDING") {
                  return (
                    <Space size={4}>
                      <Button
                        size="small"
                        type="primary"
                        onClick={() =>
                          setDecideTarget({ proposal: p, decision: "APPROVED" })
                        }
                      >
                        批准
                      </Button>
                      <Button
                        size="small"
                        danger
                        onClick={() =>
                          setDecideTarget({ proposal: p, decision: "REJECTED" })
                        }
                      >
                        拒绝
                      </Button>
                    </Space>
                  );
                }
                if (a.status === "APPROVED") {
                  if (a.manual_execution_required) {
                    return (
                      <Space size={4}>
                        <Button size="small" onClick={() => showRunbook(p)}>
                          查看运行手册
                        </Button>
                        <Tag color="blue">需人工到板端执行</Tag>
                      </Space>
                    );
                  }
                  if (a.execution_status === "NOT_EXECUTED") {
                    return (
                      <Button
                        size="small"
                        type="primary"
                        loading={busyId === p.proposal_id}
                        onClick={() => execute(p)}
                      >
                        下发执行
                      </Button>
                    );
                  }
                  return <Tag color="green">已下发 {a.queued_action_id}</Tag>;
                }
                return null;
              },
            },
          ]}
        />
      )}

      {/* 批准/拒绝弹窗 */}
      <Modal
        open={decideTarget !== null}
        title={
          decideTarget?.decision === "APPROVED" ? "批准该提案" : "拒绝该提案"
        }
        okText="提交"
        cancelText="取消"
        okButtonProps={{
          danger: decideTarget?.decision === "REJECTED",
          disabled: reason.trim().length < 8,
        }}
        onOk={submitDecision}
        onCancel={() => {
          setDecideTarget(null);
          setReason("");
        }}
      >
        <Paragraph type="secondary" style={{ fontSize: 12 }}>
          {decideTarget && describeProposal(decideTarget.proposal)}
          （If-Match: v{decideTarget?.proposal.approval?.version}）
        </Paragraph>
        <TextArea
          rows={3}
          value={reason}
          onChange={(e) => setReason(e.target.value)}
          placeholder="审批理由（至少 8 字符，将记入审批留痕）"
        />
      </Modal>

      {/* 运行手册弹窗 */}
      <Modal
        open={runbook !== null}
        title="受控运行手册（人工到板端执行）"
        footer={
          <Button onClick={() => setRunbook(null)}>关闭</Button>
        }
        onCancel={() => setRunbook(null)}
      >
        {runbook && (
          <Space orientation="vertical" size={8} style={{ width: "100%" }}>
            <Alert
              type="warning"
              showIcon
              message="此提案不会由系统自动执行"
              description="请携带以下命令到目标网关手动执行，并在工单中记录结果。"
            />
            <Paragraph code copyable>
              {runbook.command}
            </Paragraph>
            <Descriptions size="small" column={1}>
              <Descriptions.Item label="catalog_version">
                {runbook.catalog_version}
              </Descriptions.Item>
              <Descriptions.Item label="批准人">
                {runbook.approved_by ?? "—"}
              </Descriptions.Item>
              <Descriptions.Item label="批准时间">
                {runbook.approved_at
                  ? new Date(runbook.approved_at).toLocaleString()
                  : "—"}
              </Descriptions.Item>
              <Descriptions.Item label="执行状态">
                <Tag>NOT_EXECUTED</Tag>
              </Descriptions.Item>
            </Descriptions>
          </Space>
        )}
      </Modal>
    </Space>
  );
}
