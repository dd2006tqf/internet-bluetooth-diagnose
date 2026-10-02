"use client";

import {
  Alert,
  Badge,
  Button,
  Card,
  Descriptions,
  Divider,
  Drawer,
  Space,
  Spin,
  Tag,
  Typography,
  message,
} from "antd";
import { useEffect, useState } from "react";

import {
  apiClient,
  type WirelessDiagnosisResponse,
} from "@/lib/api/network";

interface WirelessDiagnosisDrawerProps {
  incidentId: string | null;
  open: boolean;
  onClose: () => void;
}

function confidenceColor(level: string) {
  if (level === "HIGH") return "green";
  if (level === "MEDIUM") return "orange";
  if (level === "LOW") return "blue";
  return "default";
}

function patternColor(pattern: string) {
  if (pattern.includes("SIMULTANEOUS") || pattern.includes("MULTI")) return "purple";
  if (pattern.includes("DEGRADATION")) return "volcano";
  if (pattern.includes("EXPLICIT")) return "cyan";
  return "default";
}

export function WirelessDiagnosisDrawer({
  incidentId,
  open,
  onClose,
}: WirelessDiagnosisDrawerProps) {
  const [data, setData] = useState<WirelessDiagnosisResponse | null>(null);
  const [loading, setLoading] = useState(false);
  const [enriching, setEnriching] = useState(false);

  useEffect(() => {
    if (!open || !incidentId) {
      setData(null);
      return;
    }
    setLoading(true);
    apiClient
      .getIncidentDiagnosis(incidentId)
      .then(setData)
      .catch((err) => {
        message.error(`获取因果诊断失败: ${err.message}`);
      })
      .finally(() => setLoading(false));
  }, [open, incidentId]);

  const handleEnrich = (force: boolean) => {
    if (!incidentId) return;
    setEnriching(true);
    apiClient
      .triggerDeepDiagnosis(incidentId, force)
      .then((res) => {
        setData(res);
        message.success("深度因果诊断与模型润色完成！");
      })
      .catch((err) => {
        message.error(`模型润色失败: ${err.message}`);
      })
      .finally(() => setEnriching(false));
  };

  return (
    <Drawer
      title="区域无线事故 AI 因果诊断详情"
      width={680}
      open={open}
      onClose={onClose}
      extra={
        <Space>
          <Button
            type="primary"
            loading={enriching}
            onClick={() => handleEnrich(true)}
          >
            重新深度分析 (Force Enrich)
          </Button>
        </Space>
      }
    >
      {loading ? (
        <div style={{ textAlign: "center", padding: "40px 0" }}>
          <Spin size="large" />
          <div style={{ marginTop: 16 }}>正在装配时空证据并执行确定性因果决裁...</div>
        </div>
      ) : data ? (
        <div style={{ display: "flex", flexDirection: "column", gap: 16 }}>
          {/* 1. 顶部概要 */}
          <Descriptions size="small" bordered column={2}>
            <Descriptions.Item label="事故标识">
              <Typography.Text code copyable>{data.incident_id}</Typography.Text>
            </Descriptions.Item>
            <Descriptions.Item label="诊断生成时间">
              {new Date(data.created_at).toLocaleString()}
            </Descriptions.Item>
            <Descriptions.Item label="算法版本">
              <Tag>{data.rules_version}</Tag>
            </Descriptions.Item>
            <Descriptions.Item label="护栏审查状态">
              <Badge
                status={data.guardrail_status === "ALLOWED" ? "success" : "warning"}
                text={data.guardrail_status}
              />
            </Descriptions.Item>
          </Descriptions>

          {/* 2. 确定性真值区 (Canonical Diagnosis) */}
          <Card
            size="small"
            title="🎯 系统判定物理事实与假说 (Canonical Ground Truth)"
            style={{ backgroundColor: "#fafafa" }}
          >
            <Space orientation="vertical" style={{ width: "100%" }}>
              <div>
                <Typography.Text strong>客观物理模式：</Typography.Text>
                <Tag color={patternColor(data.canonical.observed_pattern)}>
                  {data.canonical.observed_pattern}
                </Tag>
              </div>
              <div>
                <Typography.Text strong>疑似根因假说：</Typography.Text>
                <Tag color="geekblue" style={{ fontSize: 13, padding: "2px 8px" }}>
                  {data.canonical.hypothesis}
                </Tag>
                <Tag color={confidenceColor(data.canonical.confidence)}>
                  置信度: {data.canonical.confidence}
                </Tag>
              </div>
              <div>
                <Typography.Text strong>确定性决策依据：</Typography.Text>
                <ul style={{ margin: "4px 0 0 16px", padding: 0 }}>
                  {data.canonical.deterministic_reasons.map((r, i) => (
                    <li key={i}>{r}</li>
                  ))}
                </ul>
              </div>
              {data.canonical.device_findings && data.canonical.device_findings.length > 0 && (
                <div>
                  <Typography.Text strong>关联设备子表现：</Typography.Text>
                  <div style={{ display: "flex", flexWrap: "wrap", gap: 8, marginTop: 4 }}>
                    {data.canonical.device_findings.map((df) => (
                      <Tag key={df.device_address}>
                        {df.device_address}: {df.hypothesis} ({df.confidence})
                      </Tag>
                    ))}
                  </div>
                </div>
              )}
            </Space>
          </Card>

          {/* 3. 呈现与自然语言报告区 (Presentation) */}
          <Card
            size="small"
            title={
              <Space>
                <span>🤖 专家诊断报告与排障阐述</span>
                {data.presentation.llm_used ? (
                  <Tag color="cyan">由 {data.presentation.llm_model_name} 润色生成</Tag>
                ) : (
                  <Tag color="default">确定性模板兜底</Tag>
                )}
              </Space>
            }
          >
            <Typography.Paragraph style={{ whiteSpace: "pre-wrap", fontSize: 14 }}>
              {data.presentation.diagnosis_report}
            </Typography.Paragraph>

            {data.presentation.structured_findings &&
              data.presentation.structured_findings.length > 0 && (
                <>
                  <Divider style={{ margin: "12px 0" }}>结构化事实与证据引用 (W6 强约束)</Divider>
                  {data.presentation.structured_findings.map((sf, idx) => (
                    <div key={idx} style={{ marginBottom: 6 }}>
                      <span>• {sf.text} </span>
                      <Space orientation="horizontal" size={4}>
                        {sf.evidence_ids.map((eid) => (
                          <Tag key={eid} color="blue" style={{ fontSize: 11 }}>
                            [{eid}]
                          </Tag>
                        ))}
                      </Space>
                    </div>
                  ))}
                </>
              )}

            {data.presentation.recommendations &&
              data.presentation.recommendations.length > 0 && (
                <>
                  <Divider style={{ margin: "12px 0" }}>🛠️ 建议运维处置动作</Divider>
                  <Alert
                    type="info"
                    showIcon
                    description={
                      <ul style={{ margin: "4px 0 0 16px", padding: 0 }}>
                        {data.presentation.recommendations.map((rec, idx) => (
                          <li key={idx}>{rec}</li>
                        ))}
                      </ul>
                    }
                  />
                </>
              )}
          </Card>
        </div>
      ) : (
        <Alert type="warning" message="未能获取诊断数据" />
      )}
    </Drawer>
  );
}
