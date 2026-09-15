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
};

export type { NetworkAssetSummary, NetworkAssetDetail, TimelinePoint, CopilotAnswer };
