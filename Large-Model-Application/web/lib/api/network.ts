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
};

export type {
  NetworkAssetSummary,
  NetworkAssetDetail,
  TimelinePoint,
  CopilotAnswer,
  SiteIncidentItem,
  WirelessDeviceBaselineItem,
  CanonicalDiagnosis,
  DiagnosisPresentation,
  WirelessDiagnosisResponse,
};

