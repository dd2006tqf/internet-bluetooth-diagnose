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
  Tag,
  Timeline,
  Typography,
  message,
} from "antd";
import { ControlOutlined, ReloadOutlined } from "@ant-design/icons";
import { useEffect, useState } from "react";
import { useParams } from "next/navigation";

import { AppShell } from "@/components/AppShell";
import { apiClient } from "@/lib/api/network";
import type { NetworkAssetDetail, TimelinePoint } from "@/lib/api/network";

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
      .catch(() => {
        setDetail(null);
        setPoints([]);
      })
      .finally(() => setLoading(false));
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
            <Button icon={<ReloadOutlined />} onClick={fetchDetail}>
              刷新
            </Button>
            <Button
              type="primary"
              icon={<ControlOutlined />}
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
