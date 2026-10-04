"use client";

/**
 * Network assurance API client.
 *
 * Types are hand-written until the OpenAPI client is regenerated from the
 * server schema (scripts/generate_openapi_client.sh). They mirror
 * ``network_assurance/contracts.py`` exactly; a field drift between the two
 * surfaces as ``undefined`` in the UI rather than a type error, which is why
 * the detail page treats every field as nullable.
 */

const BASE_URL = typeof window === "undefined"
  ? "http://localhost/api/backend"
  : "/api/backend";

type TimelinePoint = {
  sequence_id: number;
  network_epoch: number;
  config_generation: number;
  captured_at: string;
  overall_state: string;
  display_score: number;
  primary_issue: string | null;
};

type NetworkAssetSummary = {
  asset_id: string;
  tenant_id: string;
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
  config_generation: number | null;
  network_epoch: number | null;
};

type NetworkAssetDetail = {
  summary: NetworkAssetSummary;
  hardware_arch: string | null;
  os_kernel: string | null;
  mac_address: string | null;
  gateway_ip: string | null;
  dns_servers: string[];
  ap_ssid: string | null;
  ap_bssid: string | null;
  rtt_interval_seconds: number | null;
  rtt_probe_targets: string[];
  latest_experience: Record<string, unknown> | null;
};

// 现场聚合查询出口（M2）：site/gateway 身份只随事实携带，展示字段从
// network_assets 富化；未注册网关的富化字段为 null（如实出现，不隐藏）。
type SiteGatewayItem = {
  gateway_id: string;
  display_name: string | null;
  connection_status: "ONLINE" | "WEAK_NET" | "OFFLINE" | null;
  last_heartbeat_at: string | null;
};

type SiteSummary = {
  site_id: string;
  gateways: SiteGatewayItem[];
};

type CopilotAnswer = {
  asset_id: string;
  overall_state: string;
  primary_issue: string | null;
  causal_chain: { step: string; explanation: string }[];
  evidence_refs: string[];
  recommended_actions: string[];
  answer: string;
  model_used: boolean;
};

// ---------------------------------------------------------------------------
// Phase 4b & 无线可视化类型定义
// ---------------------------------------------------------------------------

type SiteIncidentItem = {
  incident_id: string;
  asset_id: string;
  site_id: string;
  gateway_id: string;
  started_at_ms: number;
  last_event_ms: number;
  resolved_at_ms: number | null;
  affected_devices: number;
  state: "OPEN" | "ONGOING" | "RESOLVED";
  suspected_cause: string | null;
  evidence_event_ids: string[];
};

type WirelessDeviceBaselineItem = {
  baseline_id: string;
  asset_id: string;
  site_id: string;
  gateway_id: string;
  device_address: string;
  address_type: string;
  protocol: string;
  baseline_rssi_dbm: number | null;
  min_seen_rssi_dbm: number | null;
  max_seen_rssi_dbm: number | null;
  baseline_sample_count: number;
  state: "LEARNING" | "STABLE" | "DEGRADED";
  first_seen_ms: number | null;
  last_seen_ms: number | null;
};

type WirelessDeviceEventItem = {
  event_id: string;
  asset_id: string;
  site_id: string;
  gateway_id: string;
  protocol: string;
  device_address: string;
  address_type: string;
  hci_index: number;
  event_type: string;
  ts_ms: number;
  rssi_at_event_dbm: number | null;
  raw_reason_code: number;
  reason: string;
  source: string;
  source_detail: string;
  details_json: string;
};

type KernelProcessEntry = {
  pid: number;
  comm: string;
  tx_bytes: number;
  tx_packets: number;
  retrans_count: number;
};

type KernelDropReason = {
  reason_code: number;
  reason_name: string;
  description: string;
  protocol: string;
  count: number;
  last_timestamp_ns: number;
};

// 深度内核观测。板端"没有数据就不发"，因此两个键都可能缺席——
// 前端据此隐藏卡片，而不是渲染伪造的零值。
type KernelObservations = {
  process_top?: KernelProcessEntry[];
  skb_drop_hist?: {
    total_drops: number;
    top_reasons: KernelDropReason[];
  };
};

type CanonicalDiagnosis = {
  observed_pattern: string;
  hypothesis: string;
  confidence: "HIGH" | "MEDIUM" | "LOW" | "INSUFFICIENT";
  evidence_ids: string[];
  deterministic_reasons: string[];
  device_findings: Array<{
    device_address: string;
    observed_pattern: string;
    hypothesis: string;
    confidence: string;
    note: string;
  }>;
};

type DiagnosisPresentation = {
  llm_model_name: string | null;
  llm_used: boolean;
  diagnosis_report: string;
  structured_findings: Array<{ text: string; evidence_ids: string[] }>;
  evidence_citations: Array<{ step: string; claim: string; evidence_refs: string[] }>;
  recommendations: string[];
};

type RiskLevel = "LOW" | "MEDIUM" | "HIGH";

type RiskDriver = {
  metric: string;
  contribution_pct: number; // 产品文案："风险驱动贡献度"（不是原因占比）
  direction: "DETERIORATING" | "STABLE";
  evidence_ids: string[];
};

type TrendEvidence = {
  metric: string;
  statement: string;
  evidence_ids: string[];
};

type RiskWindow = {
  subject_type: "DEVICE" | "SITE";
  subject_id: string;
  risk_level: RiskLevel;
  window_estimation_status: "AVAILABLE" | "UNAVAILABLE";
  window_earliest_hours: number | null;
  window_latest_hours: number | null;
  forecast_horizon_hours: number;
  prediction_confidence: RiskLevel;
  data_sufficiency: "PARTIAL" | "SUFFICIENT";
  drivers: RiskDriver[];
  trend_evidence: TrendEvidence[];
  failure_criterion_id: string;
  failure_criterion_version: string;
  model_version: string;
  generated_at: string;
};

type InsufficientPrediction = {
  subject_type: "DEVICE" | "SITE";
  subject_id: string;
  data_sufficiency: "INSUFFICIENT";
  missing_requirements: string[];
  available_evidence_ids: string[];
  model_version: string;
  generated_at: string;
};

type PredictionResult = RiskWindow | InsufficientPrediction;

type WirelessDiagnosisResponse = {
  diagnosis_id: string;
  incident_id: string;
  diagnosis_version: string;
  rules_version: string;
  prompt_version: string;
  guardrail_version: string;
  created_at: string;
  canonical: CanonicalDiagnosis;
  presentation: DiagnosisPresentation;
  guardrail_status: string;
  guardrail_findings: Array<Record<string, unknown>>;
};

// ---------------------------------------------------------------------------
// ⑤ Network Operations Council —— 会商 / Policy 裁决 / 审批 / 执行
// ---------------------------------------------------------------------------

type CouncilExpertOpinion = {
  role: string;
  observations?: string[];
  referenced_fact_ids?: string[];
  recommendation_direction?: string;
};

type CouncilProposal = {
  kind: "ACTION_ID" | "CONFIG_CHANGE";
  action_id?: string | null;
  action_params?: Record<string, string>;
  config_key?: string | null;
  config_value?: string | null;
  rationale: string;
  proposed_preconditions?: string[];
  source_bindings?: Record<string, string>[];
};

type NetworkCouncilView = {
  council_id: string;
  incident_id: string;
  status: "QUEUED" | "RUNNING" | "REVIEW_PENDING" | "FAILED";
  stage: string;
  input_fingerprint: string;
  failure_code?: string | null;
  proposals?: CouncilProposal[];
  expert_opinions?: CouncilExpertOpinion[];
  requested_by_subject_id: string;
  version: number;
  created_at: string;
  updated_at: string;
};

// Policy 裁决（机器规则）与人工审批（人）是两个域——前端字段永不混用。
type ProposalPolicyDecision = {
  decision_id: string;
  allowed: boolean;
  risk: RiskLevel;
  required_preconditions: string[];
  approval_required: boolean;
  block_reason: string | null;
  execution_mode: "REMOTE_PENDING_ACTION" | "MANUAL_RUNBOOK";
  catalog_version: string;
  risk_policy_version: string;
  normalized_action: Record<string, unknown>;
};

type ProposalApprovalState = {
  approval_id: string;
  status: "PENDING" | "APPROVED" | "REJECTED" | "EXPIRED" | "SUPERSEDED";
  expires_at: string;
  version: number;
  execution_status: "NOT_EXECUTED" | "QUEUED";
  manual_execution_required: boolean;
  queued_action_id: string | null;
  approved_by: string | null;
  approved_at: string | null;
  decision_id: string;
};

type CouncilProposalState = {
  proposal_id: string;
  proposal_index: number;
  proposal_snapshot: CouncilProposal;
  decision: ProposalPolicyDecision | null;
  approval: ProposalApprovalState | null;
};

type ManualRunbook = {
  proposal_id: string;
  approval_id: string;
  command: string;
  catalog_version: string;
  approved_by: string | null;
  approved_at: string | null;
  runbook_renderer_version: string;
  manual_execution_required: true;
  execution_status: "NOT_EXECUTED";
};

type ExecuteResult = {
  execution_status: string;
  queued_action_id: string | null;
  idempotent_replay: boolean;
};


async function request<T>(path: string, init?: RequestInit): Promise<T> {
  const response = await fetch(`${BASE_URL}${path}`, {
    ...init,
    credentials: "include",
    headers: {
      ...(init?.body ? { "Content-Type": "application/json" } : {}),
      ...init?.headers,
    },
  });
  if (!response.ok) {
    throw new Error(`network api ${response.status}: ${path}`);
  }
  if (response.status === 204) return undefined as T;
  return (await response.json()) as T;
}

export const apiClient = {
  listAssets(): Promise<NetworkAssetSummary[]> {
    return request<NetworkAssetSummary[]>("/api/v1/network/assets");
  },
  listSites(): Promise<SiteSummary[]> {
    return request<SiteSummary[]>("/api/v1/network/sites");
  },
  getAsset(assetId: string): Promise<NetworkAssetDetail> {
    return request<NetworkAssetDetail>(
      `/api/v1/network/assets/${encodeURIComponent(assetId)}`,
    );
  },
  getTimeline(assetId: string, window = "15m"): Promise<{ points: TimelinePoint[] }> {
    return request(
      `/api/v1/network/assets/${encodeURIComponent(assetId)}/timeline?window=${window}`,
    );
  },
  queueAction(
    assetId: string,
    configKey: string,
    configValue: string,
  ): Promise<{ action_id: string; status: string }> {
    return request(`/api/v1/network/assets/${encodeURIComponent(assetId)}/actions`, {
      method: "POST",
      body: JSON.stringify({ config_key: configKey, config_value: configValue }),
    });
  },
  copilot(question: string): Promise<CopilotAnswer> {
    return request("/api/v1/network/copilot", {
      method: "POST",
      body: JSON.stringify({ question }),
    });
  },
  getCopilotConfig(): Promise<{
    upstream_url: string;
    model_name: string;
    has_api_key: boolean;
    api_key_masked: string;
    timeout_seconds: number;
  }> {
    return request("/api/v1/network/copilot/config");
  },
  updateCopilotConfig(payload: {
    upstream_url: string;
    model_name: string;
    api_key?: string;
    timeout_seconds?: number;
  }): Promise<{
    upstream_url: string;
    model_name: string;
    has_api_key: boolean;
    api_key_masked: string;
    timeout_seconds: number;
  }> {
    return request("/api/v1/network/copilot/config", {
      method: "POST",
      body: JSON.stringify(payload),
    });
  },
  testCopilotConfig(payload: {
    upstream_url: string;
    model_name: string;
    api_key?: string;
  }): Promise<{
    ok: boolean;
    message: string;
    latency_ms: number;
  }> {
    return request("/api/v1/network/copilot/config/test", {
      method: "POST",
      body: JSON.stringify(payload),
    });
  },

  // ---- Phase 4b & 无线可视化 API ----
  listAssetIncidents(assetId: string): Promise<SiteIncidentItem[]> {
    return request<SiteIncidentItem[]>(
      `/api/v1/network/assets/${encodeURIComponent(assetId)}/incidents`,
    );
  },
  listAssetWirelessDevices(assetId: string): Promise<WirelessDeviceBaselineItem[]> {
    return request<WirelessDeviceBaselineItem[]>(
      `/api/v1/network/assets/${encodeURIComponent(assetId)}/wireless-devices`,
    );
  },
  getAssetKernelObservations(assetId: string): Promise<KernelObservations> {
    return request<KernelObservations>(
      `/api/v1/network/assets/${encodeURIComponent(assetId)}/kernel-observations`,
    );
  },
  listAssetWirelessEvents(assetId: string): Promise<WirelessDeviceEventItem[]> {
    return request<WirelessDeviceEventItem[]>(
      `/api/v1/network/assets/${encodeURIComponent(assetId)}/wireless-events`,
    );
  },
  getIncidentDiagnosis(incidentId: string): Promise<WirelessDiagnosisResponse> {
    return request<WirelessDiagnosisResponse>(
      `/api/v1/network/assurance/incidents/${encodeURIComponent(incidentId)}/diagnosis`,
    );
  },
  triggerDeepDiagnosis(incidentId: string, force = false): Promise<WirelessDiagnosisResponse> {
    return request<WirelessDiagnosisResponse>(
      `/api/v1/network/assurance/incidents/${encodeURIComponent(incidentId)}/diagnosis?force=${force}`,
      { method: "POST" },
    );
  },
  getDeviceRiskPrediction(
    assetId: string,
    deviceAddress: string,
    windowHours = 168,
  ): Promise<PredictionResult> {
    return request<PredictionResult>(
      `/api/v1/network/assets/${encodeURIComponent(assetId)}` +
        `/wireless-devices/${encodeURIComponent(deviceAddress)}` +
        `/risk-prediction?window_hours=${windowHours}`,
    );
  },

  // ---- ⑤ Network Operations Council ----
  getIncidentCouncil(incidentId: string): Promise<NetworkCouncilView> {
    return request<NetworkCouncilView>(
      `/api/v1/network/assurance/incidents/${encodeURIComponent(incidentId)}/council`,
    );
  },
  conveneCouncil(incidentId: string, force = false): Promise<NetworkCouncilView> {
    return request<NetworkCouncilView>(
      `/api/v1/network/assurance/incidents/${encodeURIComponent(incidentId)}/council?force=${force}`,
      { method: "POST" },
    );
  },
  listCouncilProposals(incidentId: string): Promise<CouncilProposalState[]> {
    return request<CouncilProposalState[]>(
      `/api/v1/network/assurance/incidents/${encodeURIComponent(incidentId)}/council/proposals`,
    );
  },
  decideProposal(
    proposalId: string,
    decision: "APPROVED" | "REJECTED",
    reason: string,
    ifMatch: number,
  ): Promise<ProposalApprovalState> {
    return request<ProposalApprovalState>(
      `/api/v1/network/assurance/proposals/${encodeURIComponent(proposalId)}/decision`,
      {
        method: "POST",
        headers: { "If-Match": String(ifMatch) },
        body: JSON.stringify({ decision, reason }),
      },
    );
  },
  executeProposal(proposalId: string, idempotencyKey: string): Promise<ExecuteResult> {
    return request<ExecuteResult>(
      `/api/v1/network/assurance/proposals/${encodeURIComponent(proposalId)}/execute`,
      {
        method: "POST",
        headers: { "Idempotency-Key": idempotencyKey },
      },
    );
  },
  getProposalRunbook(proposalId: string): Promise<ManualRunbook> {
    return request<ManualRunbook>(
      `/api/v1/network/assurance/proposals/${encodeURIComponent(proposalId)}/runbook`,
    );
  },
};

export type {
  NetworkAssetSummary,
  NetworkAssetDetail,
  SiteSummary,
  SiteGatewayItem,
  TimelinePoint,
  CopilotAnswer,
  SiteIncidentItem,
  WirelessDeviceBaselineItem,
  WirelessDeviceEventItem,
  KernelObservations,
  KernelProcessEntry,
  KernelDropReason,
  CanonicalDiagnosis,
  DiagnosisPresentation,
  WirelessDiagnosisResponse,
  RiskLevel,
  RiskDriver,
  TrendEvidence,
  RiskWindow,
  InsufficientPrediction,
  PredictionResult,
  CouncilExpertOpinion,
  CouncilProposal,
  NetworkCouncilView,
  ProposalPolicyDecision,
  ProposalApprovalState,
  CouncilProposalState,
  ManualRunbook,
  ExecuteResult,
};

