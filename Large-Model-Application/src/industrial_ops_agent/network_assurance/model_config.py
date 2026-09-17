"""Runtime model gateway configuration for Network Copilot.

Allows hot-reloading upstream URL, model name, and API key via API/Web UI
without restarting the service.
"""

from __future__ import annotations

import os
import threading
from dataclasses import dataclass
from typing import Any
import httpx
from pydantic import BaseModel, Field


@dataclass
class HotModelConfig:
    upstream_url: str = ""
    api_key: str = ""
    model_name: str = "deepseek-v4-pro-0813"
    timeout_seconds: float = 90.0


class CopilotModelConfigResponse(BaseModel):
    upstream_url: str
    model_name: str
    has_api_key: bool
    api_key_masked: str
    timeout_seconds: float


class CopilotModelConfigRequest(BaseModel):
    upstream_url: str = Field(min_length=1, max_length=256)
    model_name: str = Field(min_length=1, max_length=128)
    api_key: str | None = Field(default=None, max_length=256)
    timeout_seconds: float = Field(default=90.0, ge=5.0, le=180.0)


class CopilotModelTestRequest(BaseModel):
    upstream_url: str
    model_name: str
    api_key: str | None = None


class CopilotModelTestResponse(BaseModel):
    ok: bool
    message: str
    latency_ms: int = 0


class RuntimeModelConfigManager:
    """Thread-safe in-memory hot configuration manager with fallback to env vars."""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._config = HotModelConfig(
            upstream_url=os.environ.get("IOAP_MODEL_GATEWAY_UPSTREAM_URL", "https://vectide.cn/v1"),
            api_key=os.environ.get("IOAP_MODEL_GATEWAY_API_KEY", ""),
            model_name=os.environ.get("IOAP_MODEL_GATEWAY_MODEL_NAME", "deepseek-v4-pro-0813"),
            timeout_seconds=float(os.environ.get("IOAP_MODEL_GATEWAY_TIMEOUT_SECONDS", "90.0")),
        )

    def get_config(self) -> HotModelConfig:
        with self._lock:
            return HotModelConfig(
                upstream_url=self._config.upstream_url,
                api_key=self._config.api_key,
                model_name=self._config.model_name,
                timeout_seconds=self._config.timeout_seconds,
            )

    def get_public_view(self) -> CopilotModelConfigResponse:
        with self._lock:
            key = self._config.api_key
            masked = ""
            if key:
                if len(key) <= 8:
                    masked = "sk-****"
                else:
                    masked = f"{key[:3]}...{key[-4:]}"
            return CopilotModelConfigResponse(
                upstream_url=self._config.upstream_url,
                model_name=self._config.model_name,
                has_api_key=bool(key),
                api_key_masked=masked,
                timeout_seconds=self._config.timeout_seconds,
            )

    def update_config(self, req: CopilotModelConfigRequest) -> CopilotModelConfigResponse:
        with self._lock:
            self._config.upstream_url = req.upstream_url.strip().rstrip("/")
            self._config.model_name = req.model_name.strip()
            self._config.timeout_seconds = req.timeout_seconds
            # 若传入了有效非空 key，则更新；若传 None 或空，保留原有 key
            if req.api_key is not None and req.api_key.strip():
                self._config.api_key = req.api_key.strip()
        return self.get_public_view()

    async def test_connection(
        self, req: CopilotModelTestRequest
    ) -> CopilotModelTestResponse:
        url = req.upstream_url.strip().rstrip("/")
        model = req.model_name.strip()
        key = req.api_key.strip() if req.api_key else ""
        if not key:
            with self._lock:
                key = self._config.api_key
        if not key:
            return CopilotModelTestResponse(ok=False, message="未配置 API Key，无法连接")

        import time

        start = time.monotonic()
        try:
            async with httpx.AsyncClient(timeout=15.0) as client:
                resp = await client.post(
                    f"{url}/chat/completions",
                    headers={"Authorization": f"Bearer {key}"},
                    json={
                        "model": model,
                        "messages": [{"role": "user", "content": "ping"}],
                        "max_tokens": 1,
                    },
                )
            latency = int((time.monotonic() - start) * 1000)
            if resp.status_code == 200:
                return CopilotModelTestResponse(
                    ok=True,
                    message=f"连接成功！模型响应耗时 {latency}ms",
                    latency_ms=latency,
                )
            error_msg = f"HTTP {resp.status_code}: {resp.text[:200]}"
            return CopilotModelTestResponse(
                ok=False, message=f"大模型中转站返回错误: {error_msg}", latency_ms=latency
            )
        except Exception as exc:
            latency = int((time.monotonic() - start) * 1000)
            return CopilotModelTestResponse(
                ok=False, message=f"连接异常: {str(exc)}", latency_ms=latency
            )


_GLOBAL_MODEL_CONFIG_MANAGER = RuntimeModelConfigManager()


def get_model_config_manager() -> RuntimeModelConfigManager:
    return _GLOBAL_MODEL_CONFIG_MANAGER
