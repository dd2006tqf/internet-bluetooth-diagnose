"use client";

import { Badge, Card, Descriptions, Progress, Table, Tag, Typography } from "antd";
import type { ColumnsType } from "antd/es/table";
import Link from "next/link";
import { useEffect, useState } from "react";

import { AppShell } from "@/components/AppShell";
import { apiClient } from "@/lib/api/network";

type NetworkAssetSummary = {
  asset_id: string;
  display_name: string | null;
  location: string | null;
  active_iface: string | null;
  link_type: string;
  ip_address: string | null;
  overall_state: string;
  primary_issue: string | null;
  display_score: number;
  connection_status: string;
  last_heartbeat_at: string | null;
};

function healthColor(state: string) {
  if (state === "GOOD") return "green";
  if (state === "DEGRADED") return "gold";
  if (state === "BAD") return "red";
  return "default";
}

function connectionColor(status: string) {
  if (status === "ONLINE") return "green";
  if (status === "WEAK_NET") return "gold";
  return "red";
}

const columns: ColumnsType<NetworkAssetSummary> = [
  {
    title: "设备",
    dataIndex: "asset_id",
    render: (_: unknown, record: NetworkAssetSummary) => (
      <Link href={`/network/${record.asset_id}`}>{record.display_name ?? record.asset_id}</Link>
    ),
  },
  {
    title: "位置",
    dataIndex: "location",
    render: (value: string | null) => value ?? "—",
  },
  {
    title: "网卡",
    dataIndex: "active_iface",
    render: (value: string | null, record: NetworkAssetSummary) =>
      value ? `${value} (${record.link_type})` : "—",
  },
  {
    title: "IP",
    dataIndex: "ip_address",
    render: (value: string | null) => value ?? "—",
  },
  {
    title: "健康",
    dataIndex: "overall_state",
    render: (state: string, record: NetworkAssetSummary) => (
      <>
        <Tag color={healthColor(state)}>{state}</Tag>
        <Progress
          type="dashboard"
          size={48}
          percent={record.display_score}
          strokeColor={record.display_score >= 70 ? "#52c41a" : record.display_score >= 40 ? "#faad14" : "#ff4d4f"}
        />
      </>
    ),
  },
  {
    title: "连接",
    dataIndex: "connection_status",
    render: (status: string) => (
      <Badge status={status === "ONLINE" ? "success" : status === "WEAK_NET" ? "warning" : "error"} />
    ),
  },
  {
    title: "根因",
    dataIndex: "primary_issue",
    ellipsis: true,
    render: (value: string | null) => value ?? "—",
  },
];

export default function NetworkAssetsPage() {
  const [assets, setAssets] = useState<NetworkAssetSummary[]>([]);
  const [loading, setLoading] = useState(true);

  useEffect(() => {
    apiClient
      .listAssets()
      .then(setAssets)
      .catch(() => setAssets([]))
      .finally(() => setLoading(false));
  }, []);

  return (
    <AppShell>
      <div className="page-stack">
        <div>
          <Typography.Title level={2}>网络资产池</Typography.Title>
          <Typography.Text type="secondary">
            边缘设备上报的不可变网络评估快照（WeakNet 单一事实源）
          </Typography.Text>
        </div>
        <Card>
          <Table
            rowKey="asset_id"
            loading={loading}
            columns={columns}
            dataSource={assets}
            pagination={false}
            locale={{ emptyText: "暂无已接入的边缘网络设备" }}
          />
        </Card>
      </div>
    </AppShell>
  );
}
