"use client";

import { Alert, Progress, Space, Tag, Tooltip, Typography } from "antd";

import type { PredictionResult, RiskLevel } from "@/lib/api/network";

const { Text } = Typography;

export function riskLevelColor(level: RiskLevel): string {
  if (level === "HIGH") return "red";
  if (level === "MEDIUM") return "orange";
  return "green";
}

export function isRiskWindow(
  result: PredictionResult,
): result is Extract<PredictionResult, { risk_level: RiskLevel }> {
  return "risk_level" in result;
}

/** 风险窗口的机器可读字段 → 人读文案（三态严格区分）。 */
export function formatWindow(result: PredictionResult): {
  text: string;
  color: string;
} {
  if (!isRiskWindow(result)) {
    return { text: "无风险判断", color: "default" };
  }
  if (result.window_estimation_status === "AVAILABLE") {
    return {
      text: `${result.window_earliest_hours}–${result.window_latest_hours} 小时`,
      color: "blue",
    };
  }
  // 风险可判但时间不可估——不编造窗口
  return { text: "窗口暂不可估", color: "default" };
}

/**
 * 单台设备的 L3 预测结果面板（契约 C 展示）。
 *
 * 严格遵循五层契约的信息纪律：不展示 HI 裸分（内部量），只展示
 * risk_level / 置信度 / 风险窗口 / 驱动贡献度 / 趋势证据。
 */
export function RiskPredictionPanel({
  deviceAddress,
  result,
}: {
  deviceAddress?: string;
  result: PredictionResult;
}) {
  if (!isRiskWindow(result)) {
    return (
      <Alert
        type="warning"
        showIcon
        message={`证据不足，未产生风险判断${deviceAddress ? `（${deviceAddress}）` : ""}`}
        description={
          <div>
            <ul style={{ margin: "4px 0 0 16px", padding: 0 }}>
              {result.missing_requirements.map((req) => (
                <li key={req}>{req}</li>
              ))}
            </ul>
            <Text type="secondary" style={{ fontSize: 12 }}>
              缺失不等于 0：系统宁可不下判断，也不编造风险。
            </Text>
          </div>
        }
      />
    );
  }

  const window = formatWindow(result);
  return (
    <div>
      <Space wrap size={[8, 8]}>
        <Tag color={riskLevelColor(result.risk_level)}>
          风险 {result.risk_level}
        </Tag>
        <Tag color="geekblue">置信度 {result.prediction_confidence}</Tag>
        <Tooltip
          title={
            result.window_estimation_status === "AVAILABLE"
              ? `失效判据 ${result.failure_criterion_id} ${result.failure_criterion_version} 按组件外推取交集；预测视野 ${result.forecast_horizon_hours}h`
              : "风险可判，但退化速度不足以可靠外推时间窗（不以风险等级硬映射时间）"
          }
        >
          <Tag color={window.color}>{window.text}</Tag>
        </Tooltip>
        <Tag color={result.data_sufficiency === "SUFFICIENT" ? "green" : "orange"}>
          数据 {result.data_sufficiency}
        </Tag>
      </Space>

      {result.drivers.length > 0 ? (
        <div style={{ marginTop: 12 }}>
          <Text strong>风险驱动贡献度</Text>
          {result.drivers.map((d) => (
            <div
              key={d.metric}
              style={{ display: "flex", alignItems: "center", gap: 8, marginTop: 6 }}
            >
              <Text code style={{ width: 150 }}>
                {d.metric}
              </Text>
              <Progress
                percent={Number(d.contribution_pct.toFixed(1))}
                size="small"
                style={{ flex: 1, marginBottom: 0 }}
                strokeColor={d.direction === "DETERIORATING" ? "#ff4d4f" : "#d9d9d9"}
              />
              <Tag color={d.direction === "DETERIORATING" ? "volcano" : "default"}>
                {d.direction === "DETERIORATING" ? "恶化中" : "平稳"}
              </Tag>
            </div>
          ))}
        </div>
      ) : (
        <div style={{ marginTop: 12 }}>
          <Text type="secondary">当前无显著恶化驱动（各指标平稳）</Text>
        </div>
      )}

      {result.trend_evidence.length > 0 && (
        <div style={{ marginTop: 8 }}>
          <Text type="secondary" style={{ fontSize: 12 }}>
            {result.trend_evidence.map((t) => t.statement).join("；")}
          </Text>
        </div>
      )}

      <div style={{ marginTop: 6 }}>
        <Text type="secondary" style={{ fontSize: 11 }}>
          模型 {result.model_version} · 判据 {result.failure_criterion_id}{" "}
          {result.failure_criterion_version}
        </Text>
      </div>
    </div>
  );
}
