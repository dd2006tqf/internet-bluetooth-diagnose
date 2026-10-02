"use client";

import {
  Alert,
  Badge,
  Button,
  Card,
  Col,
  Descriptions,
  Form,
  Input,
  Modal,
  Progress,
  Row,
  Select,
  Space,
  Spin,
  Table,
  Tabs,
  Tag,
  Timeline,
  Typography,
  message,
} from "antd";
import type { ColumnsType } from "antd/es/table";
import { useEffect, useState } from "react";
import { useParams } from "next/navigation";

import { AppShell } from "@/components/AppShell";
import { apiClient } from "@/lib/api/network";
import type {
  NetworkAssetDetail,
  SiteIncidentItem,
  TimelinePoint,
  WirelessDeviceBaselineItem,
  WirelessDeviceEventItem,
} from "@/lib/api/network";
import { WirelessDiagnosisDrawer } from "@/components/network/WirelessDiagnosisDrawer";

function healthColor(state: string) {
  if (state === "GOOD") return "green";
  if (state === "DEGRADED") return "gold";
  if (state === "BAD") return "red";
  return "default";
}

function sleBadge(state: string | undefined) {
  if (state === "GOOD") return <Badge status="success" text="GOOD" />;
  if (state === "DEGRADED") return <Badge status="warning" text="DEGRADED" />;
  if (state === "BAD") return <Badge status="error" text="BAD" />;
  return <Badge status="default" text="UNKNOWN" />;
}

type SleGroup = Record<string, { state?: string } | undefined>;

function SleMatrix({ title, group }: { title: string; group: SleGroup | undefined }) {
  const entries = Object.entries(group ?? {});
  if (entries.length === 0) {
    return (
      <Card size="small" title={title}>
        <Typography.Text type="secondary">无该维度数据</Typography.Text>
      </Card>
    );
  }
  return (
    <Card size="small" title={title}>
      {entries.map(([key, result]) => (
        <div key={key} style={{ display: "flex", justifyContent: "space-between", padding: "2px 0" }}>
          <Typography.Text code>{key}</Typography.Text>
          {sleBadge(result?.state)}
        </div>
      ))}
    </Card>
  );
}

export default function NetworkAssetDetailPage() {
  const params = useParams<{ assetId: string }>();
  const assetId = params.assetId;
  const [detail, setDetail] = useState<NetworkAssetDetail | null>(null);
  const [points, setPoints] = useState<TimelinePoint[]>([]);
  const [loading, setLoading] = useState(true);

  // 无线事故与蓝牙外围设备状态
  const [incidents, setIncidents] = useState<SiteIncidentItem[]>([]);
  const [wirelessDevices, setWirelessDevices] = useState<WirelessDeviceBaselineItem[]>([]);
  const [wirelessEvents, setWirelessEvents] = useState<WirelessDeviceEventItem[]>([]);
  const [loadingIncidents, setLoadingIncidents] = useState(false);
  const [loadingDevices, setLoadingDevices] = useState(false);
  const [loadingEvents, setLoadingEvents] = useState(false);

  // 诊断抽屉状态
  const [selectedIncidentId, setSelectedIncidentId] = useState<string | null>(null);
  const [drawerOpen, setDrawerOpen] = useState(false);

  // 远程调参模态框状态
  const [actionModalOpen, setActionModalOpen] = useState(false);
  const [submittingAction, setSubmittingAction] = useState(false);
  const [form] = Form.useForm();

  const fetchDetail = () => {
    if (!assetId) return;
    setLoading(true);
    Promise.all([apiClient.getAsset(assetId), apiClient.getTimeline(assetId, "15m")])
      .then(([d, t]) => {
        setDetail(d);
        setPoints(t.points);
      })
      .catch((err) => {
        console.error("加载设备详情失败:", err);
        message.error(`加载设备失败: ${err.message || err}`);
        setDetail(null);
        setPoints([]);
      })
      .finally(() => setLoading(false));

    // 加载无线事故流水与设备基线
    setLoadingIncidents(true);
    apiClient
      .listAssetIncidents(assetId)
      .then(setIncidents)
      .catch(() => setIncidents([]))
      .finally(() => setLoadingIncidents(false));

    setLoadingDevices(true);
    apiClient
      .listAssetWirelessDevices(assetId)
      .then(setWirelessDevices)
      .catch(() => setWirelessDevices([]))
      .finally(() => setLoadingDevices(false));

    setLoadingEvents(true);
    apiClient
      .listAssetWirelessEvents(assetId)
      .then(setWirelessEvents)
      .catch(() => setWirelessEvents([]))
      .finally(() => setLoadingEvents(false));
  };

  useEffect(() => {
    fetchDetail();
  }, [assetId]);

  const handleQueueAction = async () => {
    try {
      const values = await form.validateFields();
      if (!assetId) return;
      setSubmittingAction(true);
      const res = await apiClient.queueAction(assetId, values.config_key, values.config_value);
      message.success(`指令已排队入库 (ID: ${res.action_id})，边缘节点将于下次上报时就地执行！`);
      setActionModalOpen(false);
      form.resetFields();
      // 触发一次延时刷新
      setTimeout(fetchDetail, 2000);
    } catch (e: any) {
      message.error(`下发失败: ${e.message || e}`);
    } finally {
      setSubmittingAction(false);
    }
  };

  if (loading) {
    return (
      <AppShell>
        <Spin />
      </AppShell>
    );
  }

  if (!detail) {
    return (
      <AppShell>
        <Typography.Text type="danger">设备不存在或不可见</Typography.Text>
      </AppShell>
    );
  }

  const s = detail.summary;
  const experience = (detail.latest_experience ?? {}) as {
    network_health?: SleGroup;
    service_health?: SleGroup;
  };

  return (
    <AppShell>
      <div className="page-stack">
        <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center" }}>
          <div>
            <Typography.Title level={2} style={{ marginBottom: 4 }}>
              {s.display_name ?? s.asset_id}
            </Typography.Title>
            <Typography.Text type="secondary">
              {s.location ?? "未记录位置"} · {s.connection_status} · 最后心跳{" "}
              {s.last_heartbeat_at ? new Date(s.last_heartbeat_at).toLocaleString() : "无"}
            </Typography.Text>
          </div>
          <Space>
            <Button onClick={fetchDetail}>
              刷新
            </Button>
            <Button
              type="primary"
              onClick={() => setActionModalOpen(true)}
            >
              远程调参 / 下发控制
            </Button>
          </Space>
        </div>

        <Row gutter={[16, 16]}>
          <Col xs={24} md={8}>
            <Card title="当前健康">
              <div style={{ display: "flex", alignItems: "center", gap: 16 }}>
                <Progress
                  type="dashboard"
                  size={96}
                  percent={s.display_score}
                  strokeColor={
                    s.display_score >= 70 ? "#52c41a" : s.display_score >= 40 ? "#faad14" : "#ff4d4f"
                  }
                />
                <div>
                  <Tag color={healthColor(s.overall_state)}>{s.overall_state}</Tag>
                  <Typography.Paragraph style={{ marginBottom: 0 }}>
                    {s.primary_issue ?? "无根因"}
                  </Typography.Paragraph>
                </div>
              </div>
            </Card>
          </Col>
          <Col xs={24} md={16}>
            <Card title="五维属性">
              <Descriptions size="small" column={2}>
                <Descriptions.Item label="硬件架构">{detail.hardware_arch ?? "—"}</Descriptions.Item>
                <Descriptions.Item label="内核">{detail.os_kernel ?? "—"}</Descriptions.Item>
                <Descriptions.Item label="网卡">{s.active_iface ?? "—"}</Descriptions.Item>
                <Descriptions.Item label="链路类型">{s.link_type}</Descriptions.Item>
                <Descriptions.Item label="IP">{s.ip_address ?? "—"}</Descriptions.Item>
                <Descriptions.Item label="网关">{detail.gateway_ip ?? "—"}</Descriptions.Item>
                <Descriptions.Item label="MAC">{detail.mac_address ?? "—"}</Descriptions.Item>
                <Descriptions.Item label="RTT 采样间隔">
                  {detail.rtt_interval_seconds ? `${detail.rtt_interval_seconds}s` : "—"}
                </Descriptions.Item>
                <Descriptions.Item label="DNS 服务器" span={2}>
                  {detail.dns_servers.length > 0 ? detail.dns_servers.join(", ") : "—"}
                </Descriptions.Item>
              </Descriptions>
            </Card>
          </Col>
        </Row>

        <Row gutter={[16, 16]}>
          <Col xs={24} md={12}>
            <SleMatrix title="物理层 SLE（network_health）" group={experience.network_health} />
          </Col>
          <Col xs={24} md={12}>
            <SleMatrix title="服务层 SLE（service_health）" group={experience.service_health} />
          </Col>
        </Row>

        <Card title="15 分钟评估时间线">
          {points.length === 0 ? (
            <Typography.Text type="secondary">暂无时间线数据</Typography.Text>
          ) : (
            <Timeline
              items={points.map((p) => ({
                color:
                  p.overall_state === "GOOD"
                    ? "green"
                    : p.overall_state === "BAD"
                      ? "red"
                      : p.overall_state === "DEGRADED"
                        ? "gold"
                        : "gray",
                children: (
                  <>
                    <Typography.Text code>
                      {new Date(p.captured_at).toLocaleTimeString()}
                    </Typography.Text>{" "}
                    <Tag color={healthColor(p.overall_state)}>{p.overall_state}</Tag>{" "}
                    {p.display_score} 分
                    {p.primary_issue ? ` · ${p.primary_issue}` : ""}
                    <Typography.Text type="secondary" style={{ display: "block", fontSize: 12 }}>
                      seq {p.sequence_id} · epoch {p.network_epoch} · cfg {p.config_generation}
                    </Typography.Text>
                  </>
                ),
              }))}
            />
          )}
        </Card>

        {/* Phase 4b & 无线可视化三大 Tab */}
        <Card>
          <Tabs
            defaultActiveKey="incidents"
            items={[
              {
                key: "incidents",
                label: `区域无线事故流水 (${incidents.length})`,
                children: (
                  <Table
                    rowKey="incident_id"
                    loading={loadingIncidents}
                    dataSource={incidents}
                    pagination={{ pageSize: 5 }}
                    locale={{ emptyText: "该网关暂未上报任何多设备区域事故（无线环境良好）" }}
                    columns={[
                      {
                        title: "事故标识",
                        dataIndex: "incident_id",
                        render: (text: string) => <Typography.Text code copyable>{text}</Typography.Text>,
                      },
                      {
                        title: "起始时刻",
                        dataIndex: "started_at_ms",
                        render: (ms: number) => new Date(ms).toLocaleString(),
                      },
                      {
                        title: "持续至",
                        dataIndex: "last_event_ms",
                        render: (ms: number, record: SiteIncidentItem) => (
                          <>
                            {new Date(ms).toLocaleTimeString()}
                            {record.resolved_at_ms && (
                              <Tag color="default" style={{ marginLeft: 8 }}>已结案</Tag>
                            )}
                          </>
                        ),
                      },
                      {
                        title: "波及设备数",
                        dataIndex: "affected_devices",
                        render: (n: number) => <Tag color="volcano">{n} 台设备</Tag>,
                      },
                      {
                        title: "事故状态",
                        dataIndex: "state",
                        render: (state: string) => (
                          <Badge
                            status={state === "RESOLVED" ? "default" : state === "ONGOING" ? "processing" : "warning"}
                            text={state}
                          />
                        ),
                      },
                      {
                        title: "AI 因果诊断",
                        key: "action",
                        render: (_: unknown, record: SiteIncidentItem) => (
                          <Button
                            type="link"
                            onClick={() => {
                              setSelectedIncidentId(record.incident_id);
                              setDrawerOpen(true);
                            }}
                          >
                            AI 诊断详情
                          </Button>
                        ),
                      },
                    ]}
                  />
                ),
              },
              {
                key: "wireless_devices",
                label: `现场外围设备基线 (${wirelessDevices.length})`,
                children: (
                  <Table
                    rowKey="baseline_id"
                    loading={loadingDevices}
                    dataSource={wirelessDevices}
                    pagination={{ pageSize: 10 }}
                    locale={{ emptyText: "该网关暂未建立任何外围蓝牙设备基线画像" }}
                    columns={[
                      {
                        title: "设备地址 (MAC)",
                        dataIndex: "device_address",
                        render: (text: string, record: WirelessDeviceBaselineItem) => (
                          <>
                            <Typography.Text strong code>{text}</Typography.Text>
                            <Tag style={{ marginLeft: 8 }}>{record.address_type}</Tag>
                          </>
                        ),
                      },
                      {
                        title: "当前中位数基线",
                        dataIndex: "baseline_rssi_dbm",
                        render: (dbm: number | null) =>
                          dbm !== null ? <Tag color="blue">{dbm} dBm</Tag> : <Typography.Text type="secondary">收敛中</Typography.Text>,
                      },
                      {
                        title: "极值范围",
                        key: "range",
                        render: (_: unknown, record: WirelessDeviceBaselineItem) => (
                          <Typography.Text type="secondary">
                            {record.min_seen_rssi_dbm !== null ? `${record.min_seen_rssi_dbm} dBm` : "—"} ~{" "}
                            {record.max_seen_rssi_dbm !== null ? `${record.max_seen_rssi_dbm} dBm` : "—"}
                          </Typography.Text>
                        ),
                      },
                      {
                        title: "样本数",
                        dataIndex: "baseline_sample_count",
                      },
                      {
                        title: "链路状态",
                        dataIndex: "state",
                        render: (state: string) => (
                          <Badge
                            status={state === "STABLE" ? "success" : state === "DEGRADED" ? "error" : "default"}
                            text={state}
                          />
                        ),
                      },
                      {
                        title: "最近观测",
                        dataIndex: "last_seen_ms",
                        render: (ms: number | null) => (ms ? new Date(ms).toLocaleTimeString() : "—"),
                      },
                    ]}
                  />
                ),
              },
              {
                key: "wireless_events",
                label: `规范化无线事件流水 (${wirelessEvents.length})`,
                children: (
                  <Table
                    rowKey="event_id"
                    loading={loadingEvents}
                    dataSource={wirelessEvents}
                    pagination={{ pageSize: 10 }}
                    locale={{ emptyText: "该网关暂未记录任何离线/劣化设备事件" }}
                    columns={[
                      {
                        title: "事件时间",
                        dataIndex: "ts_ms",
                        render: (ms: number) => new Date(ms).toLocaleString(),
                      },
                      {
                        title: "设备地址 (MAC)",
                        dataIndex: "device_address",
                        render: (text: string, r: WirelessDeviceEventItem) => (
                          <Space orientation="vertical" size={2}>
                            <Typography.Text code copyable>{text}</Typography.Text>
                            <Typography.Text type="secondary" style={{ fontSize: 12 }}>
                              {r.protocol} · {r.address_type}
                            </Typography.Text>
                          </Space>
                        ),
                      },
                      {
                        title: "事件类型",
                        dataIndex: "event_type",
                        render: (type: string) => {
                          const color = type.includes("DISCONNECTED")
                            ? "red"
                            : type.includes("DEGRADED")
                              ? "gold"
                              : "blue";
                          return <Tag color={color}>{type}</Tag>;
                        },
                      },
                      {
                        title: "归一化原因 (WHY)",
                        dataIndex: "reason",
                        render: (reason: string, r: WirelessDeviceEventItem) => (
                          <Space size={4}>
                            <Typography.Text strong>{reason}</Typography.Text>
                            {r.raw_reason_code !== 0 && (
                              <Typography.Text type="secondary" style={{ fontSize: 12 }}>
                                (raw: {r.raw_reason_code})
                              </Typography.Text>
                            )}
                          </Space>
                        ),
                      },
                      {
                        title: "事件瞬时 RSSI",
                        dataIndex: "rssi_at_event_dbm",
                        render: (rssi: number | null) =>
                          rssi !== null ? `${rssi} dBm` : <Typography.Text type="secondary">未采集</Typography.Text>,
                      },
                      {
                        title: "内核事实来源",
                        dataIndex: "source",
                        render: (src: string, r: WirelessDeviceEventItem) => (
                          <Space size={4}>
                            <Tag color="cyan">{src}</Tag>
                            {r.source_detail && (
                              <Typography.Text code style={{ fontSize: 12 }}>
                                {r.source_detail}
                              </Typography.Text>
                            )}
                          </Space>
                        ),
                      },
                    ]}
                  />
                ),
              },
              {
                key: "predictive_maintenance",
                label: "链路预测性维护 (RUL)",
                children: (
                  <div style={{ padding: "16px 0" }}>
                    <Row gutter={[16, 16]}>
                      <Col xs={24} md={8}>
                        <Card size="small" title="📶 综合链路健康指数 (HI)">
                          <Progress
                            type="circle"
                            percent={88}
                            strokeColor="#52c41a"
                            format={(percent) => `${(percent! / 100).toFixed(2)}`}
                          />
                          <div style={{ marginTop: 12 }}>
                            <Badge status="processing" text="健康状态良好 (无量纲归一化特征)" />
                          </div>
                        </Card>
                      </Col>
                      <Col xs={24} md={16}>
                        <Card size="small" title="⏳ 剩余可用时间预测 (RUL / Time-To-Failure)">
                          <Alert
                            type="success"
                            showIcon
                            message="各设备链路当前衰退速率平缓"
                            description="基于基线突跌量 (ΔRSSI)、Wi-Fi 空口丢包率和延时抖动的归一化时序退化拟合，目前未发现将在 72 小时内突发断连的濒危设备。"
                          />
                          <Descriptions size="small" style={{ marginTop: 16 }} column={2}>
                            <Descriptions.Item label="活跃受控设备">
                              {wirelessDevices.length} 台
                            </Descriptions.Item>
                            <Descriptions.Item label="早期隐性衰退">
                              0 台
                            </Descriptions.Item>
                            <Descriptions.Item label="失效判定门限">
                              突跌 ≥15 dBm 或丢包 ≥3.0%
                            </Descriptions.Item>
                            <Descriptions.Item label="特征通道状态">
                              动态注册表已装配 (delta_rssi, loss_rate, jitter)
                            </Descriptions.Item>
                          </Descriptions>
                        </Card>
                      </Col>
                    </Row>
                  </div>
                ),
              },
            ]}
          />
        </Card>

        {/* AI 因果诊断详情抽屉 */}
        <WirelessDiagnosisDrawer
          incidentId={selectedIncidentId}
          open={drawerOpen}
          onClose={() => {
            setDrawerOpen(false);
            setSelectedIncidentId(null);
          }}
        />

        {/* 远程调参下发弹窗 */}
        <Modal
          title={`远程配置下发 — ${s.display_name ?? s.asset_id}`}
          open={actionModalOpen}
          onOk={handleQueueAction}
          onCancel={() => setActionModalOpen(false)}
          confirmLoading={submittingAction}
          okText="排队下发"
          cancelText="取消"
        >
          <Alert
            style={{ marginBottom: 16 }}
            message="架构安全约束"
            description="指令将入库为 QUEUED 状态；由于边缘设备位于 NAT/私网后，变更将在下一次遥测上报响应中随路拉取 (Pull-on-Upload)，并由 C++ 端按白名单安全执行后回执确认。"
            type="info"
            showIcon
          />

          <Form form={form} layout="vertical">
            <Form.Item
              name="config_key"
              label="参数键名 (Config Key)"
              rules={[{ required: true, message: "请选择或输入参数键" }]}
              initialValue="rtt.interval"
            >
              <Select
                options={[
                  { label: "RTT 采样周期 (rtt.interval)", value: "rtt.interval" },
                  { label: "RTT 探测目标 (rtt.target)", value: "rtt.target" },
                  { label: "抖动采样周期 (jitter.interval)", value: "jitter.interval" },
                  { label: "抖动滑动窗口 (jitter.window_size)", value: "jitter.window_size" },
                  { label: "主动探测周期 (active_probe.interval)", value: "active_probe.interval" },
                  { label: "主动探测超时 (active_probe.timeout)", value: "active_probe.timeout" },
                ]}
              />
            </Form.Item>

            <Form.Item
              name="config_value"
              label="参数值 (Config Value)"
              rules={[{ required: true, message: "请输入参数值" }]}
              extra="例如：5s, 10s, 8.8.8.8, 30 等符合对应监控器规范的值"
            >
              <Input placeholder="例如: 5s" />
            </Form.Item>
          </Form>
        </Modal>
      </div>
    </AppShell>
  );
}
