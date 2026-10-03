"use client";

import {
  Alert,
  Badge,
  Button,
  Card,
  Descriptions,
  Divider,
  Space,
  Spin,
  Table,
  Tag,
  Typography,
  message,
} from "antd";
import Link from "next/link";
import { useParams } from "next/navigation";
import { useCallback, useEffect, useState } from "react";

import { AppShell } from "@/components/AppShell";
import { RiskPredictionPanel } from "@/components/network/RiskPredictionPanel";
import { apiClient } from "@/lib/api/network";
import type {
  PredictionResult,
  SiteIncidentItem,
  WirelessDeviceEventItem,
  WirelessDiagnosisResponse,
} from "@/lib/api/network";

const { Title, Text, Paragraph } = Typography;

function stateBadge(state: string) {
  if (state === "RESOLVED") return <Badge status="default" text={state} />;
  if (state === "ONGOING") return <Badge status="processing" text={state} />;
  return <Badge status="warning" text={state} />;
}

function confidenceColor(level: string): string {
  if (level === "HIGH") return "green";
  if (level === "MEDIUM") return "orange";
  return "blue";
}

/**
 * Incident Detail Page —— 整个 WeakNet 的核心现场视图。
 *
 * 信息呈现顺序是五层契约的硬约束（展示与人工交互平面不产生任何推断）：
 *   WHAT（事实/影响） → WHY（模式/假说） → EVIDENCE（证据）
 *   → AI 解释 → WHAT NEXT（建议动作） → FUTURE RISK（未来风险）
 */
export default function IncidentDetailPage() {
  const params = useParams<{ assetId: string; incidentId: string }>();
  const { assetId, incidentId } = params;

  const [incident, setIncident] = useState<SiteIncidentItem | null>(null);
  const [notFound, setNotFound] = useState(false);
  const [events, setEvents] = useState<WirelessDeviceEventItem[]>([]);
  const [diagnosis, setDiagnosis] = useState<WirelessDiagnosisResponse | null>(null);
  const [diagnosisError, setDiagnosisError] = useState<string | null>(null);
  const [risks, setRisks] = useState<Record<string, PredictionResult>>({});
  const [loading, setLoading] = useState(true);
  const [enriching, setEnriching] = useState(false);

  const fetchDetail = useCallback(() => {
    if (!assetId || !incidentId) return;
    setLoading(true);
    apiClient
      .listAssetIncidents(assetId)
      .then((incidents) => {
        const found = incidents.find((i) => i.incident_id === incidentId) ?? null;
        setIncident(found);
        setNotFound(found === null);
        if (!found) return null;
        return apiClient.listAssetWirelessEvents(assetId).then((evs) => {
          const evidenceSet = new Set(found.evidence_event_ids);
          setEvents(evs.filter((e) => evidenceSet.has(e.event_id)));
          return found;
        });
      })
      .then((found) => {
        if (!found) return;
        // 诊断：GET 语义下服务端会就地生成确定性诊断（零模型成本）
        apiClient
          .getIncidentDiagnosis(found.incident_id)
          .then((d) => {
            setDiagnosis(d);
            setDiagnosisError(null);
          })
          .catch((err: Error) => {
            setDiagnosisError(err.message || "诊断暂不可用");
          });
      })
      .catch(() => setNotFound(true))
      .finally(() => setLoading(false));
  }, [assetId, incidentId]);

  // 设备清单与风险预测依赖 events（证据过滤后的设备地址）
  useEffect(() => {
    fetchDetail();
  }, [fetchDetail]);

  useEffect(() => {
    if (!incident || events.length === 0 || !assetId) return;
    const affected = Array.from(new Set(events.map((e) => e.device_address))).slice(0, 8);
    if (affected.length === 0) return;
    let cancelled = false;
    Promise.all(
      affected.map((addr) =>
        apiClient
          .getDeviceRiskPrediction(assetId, addr)
          .then((r) => [addr, r] as const)
          .catch(() => [addr, null] as const),
      ),
    ).then((pairs) => {
      if (cancelled) return;
      const map: Record<string, PredictionResult> = {};
      for (const [addr, r] of pairs) {
        if (r) map[addr] = r;
      }
      setRisks(map);
    });
    return () => {
      cancelled = true;
    };
  }, [incident, events, assetId]);

  const handleDeepDiagnose = async () => {
    if (!incident) return;
    setEnriching(true);
    try {
      const res = await apiClient.triggerDeepDiagnosis(incident.incident_id);
      setDiagnosis(res);
      message.success("深度诊断完成（大模型润色已更新）");
    } catch (err) {
      message.error(`深度诊断失败: ${(err as Error).message}`);
    } finally {
      setEnriching(false);
    }
  };

  if (loading) {
    return (
      <AppShell>
        <Spin />
      </AppShell>
    );
  }

  if (notFound || !incident) {
    return (
      <AppShell>
        <div className="page-stack">
          <Alert
            type="warning"
            message="事故不存在或不可见"
            description={`未在该网关下找到 ${incidentId}`}
          />
          <Link href={`/network/${assetId}`}>← 返回网关详情</Link>
        </div>
      </AppShell>
    );
  }

  const durationSec = Math.max(
    0,
    Math.round((incident.last_event_ms - incident.started_at_ms) / 1000),
  );
  const canonical = diagnosis?.canonical ?? null;
  const presentation = diagnosis?.presentation ?? null;
  const affectedDevices = Array.from(new Set(events.map((e) => e.device_address)));

  return (
    <AppShell>
      <div className="page-stack">
        <div>
          <Link href={`/network/${assetId}`}>← 返回网关详情</Link>
          <Title level={3} style={{ marginTop: 8, marginBottom: 4 }}>
            区域无线事故 <Text code>{incident.incident_id.slice(0, 28)}…</Text>{" "}
            {stateBadge(incident.state)}
          </Title>
          <Text type="secondary">
            {new Date(incident.started_at_ms).toLocaleString()} —{" "}
            {new Date(incident.last_event_ms).toLocaleTimeString()} · 持续 {durationSec} 秒 ·
            波及 {incident.affected_devices} 台设备
          </Text>
        </div>

        {/* 1. WHAT HAPPENED —— 事实与影响，永远第一眼 */}
        <Card title="① 发生了什么（事实）" size="small">
          <Descriptions size="small" column={3}>
            <Descriptions.Item label="事故状态">
              {stateBadge(incident.state)}
            </Descriptions.Item>
            <Descriptions.Item label="持续时长">{durationSec} 秒</Descriptions.Item>
            <Descriptions.Item label="波及设备">
              <Tag color="volcano">{incident.affected_devices} 台</Tag>
            </Descriptions.Item>
            <Descriptions.Item label="起始">
              {new Date(incident.started_at_ms).toLocaleString()}
            </Descriptions.Item>
            <Descriptions.Item label="最近异常">
              {new Date(incident.last_event_ms).toLocaleString()}
            </Descriptions.Item>
            <Descriptions.Item label="结案时刻">
              {incident.resolved_at_ms
                ? new Date(incident.resolved_at_ms).toLocaleString()
                : "进行中"}
            </Descriptions.Item>
          </Descriptions>
        </Card>

        {/* 2. WHY —— 确定性模式与推断假说 */}
        <Card title="② 为什么（确定性模式 + 推断假说）" size="small">
          {!canonical ? (
            <Text type="secondary">
              {diagnosisError ? `诊断暂不可用：${diagnosisError}` : "正在装配证据…"}
            </Text>
          ) : (
            <Space orientation="vertical" size={8} style={{ width: "100%" }}>
              <div>
                <Text strong>物理模式：</Text>
                <Tag color="purple">{canonical.observed_pattern}</Tag>
                <Text strong style={{ marginLeft: 16 }}>
                  推断假说：
                </Text>
                <Tag color="geekblue" style={{ fontSize: 13 }}>
                  {canonical.hypothesis}
                </Tag>
                <Tag color={confidenceColor(canonical.confidence)}>
                  置信度 {canonical.confidence}
                </Tag>
              </div>
              <ul style={{ margin: 0, paddingLeft: 20 }}>
                {canonical.deterministic_reasons.map((r) => (
                  <li key={r}>
                    <Text>{r}</Text>
                  </li>
                ))}
              </ul>
            </Space>
          )}
        </Card>

        {/* 3. EVIDENCE —— 构成事故的原始事件 */}
        <Card title={`③ 证据（${events.length} 条现场事件）`} size="small">
          <Table
            rowKey="event_id"
            size="small"
            dataSource={events}
            pagination={{ pageSize: 5, hideOnSinglePage: true }}
            locale={{ emptyText: "证据事件已超出在线窗口（可查数据库历史）" }}
            columns={[
              {
                title: "时刻",
                dataIndex: "ts_ms",
                width: 170,
                render: (ts: number) => new Date(ts).toLocaleTimeString(),
              },
              {
                title: "设备",
                dataIndex: "device_address",
                render: (a: string) => <Text code>{a}</Text>,
              },
              { title: "事件", dataIndex: "event_type" },
              { title: "原因", dataIndex: "reason" },
              {
                title: "断开时信号",
                dataIndex: "rssi_at_event_dbm",
                render: (v: number | null) =>
                  v === null ? (
                    <Text type="secondary">未采集</Text>
                  ) : (
                    <Tag>{v} dBm</Tag>
                  ),
              },
            ]}
          />
        </Card>

        {/* 4. AI 解释 —— 排在事实与证据之后 */}
        <Card
          title="④ AI 专家阐述"
          size="small"
          extra={
            <Space>
              {presentation?.llm_used && (
                <Tag color="cyan">{presentation.llm_model_name} 润色</Tag>
              )}
              {diagnosis && (
                <Tag
                  color={diagnosis.guardrail_status === "ALLOWED" ? "green" : "orange"}
                >
                  护栏 {diagnosis.guardrail_status}
                </Tag>
              )}
              <Button size="small" loading={enriching} onClick={handleDeepDiagnose}>
                重新深度分析
              </Button>
            </Space>
          }
        >
          {presentation ? (
            <Paragraph style={{ whiteSpace: "pre-wrap" }}>
              {presentation.diagnosis_report}
            </Paragraph>
          ) : (
            <Text type="secondary">暂无阐述（确定性诊断存在时会自动带出）</Text>
          )}
        </Card>

        {/* 5. WHAT NEXT —— 建议动作 */}
        <Card title="⑤ 建议动作" size="small">
          {presentation && presentation.recommendations.length > 0 ? (
            <Alert
              type="info"
              showIcon
              message="建议（未执行，仅提议）"
              description={
                <ul style={{ margin: "4px 0 0 16px", padding: 0 }}>
                  {presentation.recommendations.map((r) => (
                    <li key={r}>{r}</li>
                  ))}
                </ul>
              }
            />
          ) : (
            <Text type="secondary">暂无建议动作</Text>
          )}
        </Card>

        {/* 6. FUTURE RISK —— L3 预测（按受影响设备） */}
        <Card title="⑥ 未来风险（受影响设备）" size="small">
          {affectedDevices.length === 0 ? (
            <Text type="secondary">无受影响设备的可查证据</Text>
          ) : (
            <Space orientation="vertical" size={12} style={{ width: "100%" }}>
              {affectedDevices.map((addr) => (
                <Card
                  key={addr}
                  size="small"
                  type="inner"
                  title={<Text code>{addr}</Text>}
                >
                  {risks[addr] ? (
                    <RiskPredictionPanel deviceAddress={addr} result={risks[addr]} />
                  ) : (
                    <Text type="secondary">风险预测加载中…</Text>
                  )}
                </Card>
              ))}
            </Space>
          )}
        </Card>

        <Divider />
        <Text type="secondary" style={{ fontSize: 12 }}>
          本页只展示各层产物：事实来自 L1，模式与假说来自 L2（确定性规则先决裁），
          未来风险来自 L3（无模型调用），建议仅为提议——任何动作须经人工审批后才可能下发。
        </Text>
      </div>
    </AppShell>
  );
}
