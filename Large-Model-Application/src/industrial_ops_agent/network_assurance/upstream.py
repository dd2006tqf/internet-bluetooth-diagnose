"""Shared upstream HTTP model communication utility.

Extracted from copilot.py to deduplicate OpenAI-compatible upstream calls
and unify json stripping, timeout handling, and reasoning token fallbacks.
"""

from __future__ import annotations

import json
import os
from typing import Any

import httpx

from industrial_ops_agent.network_assurance.model_config import get_model_config_manager

_DEFAULT_INFERENCE_TIMEOUT_SECONDS = 90.0


def complete_json(
    system_instruction: str,
    user_content: str,
    *,
    max_tokens: int = 2048,
    timeout_seconds: float = _DEFAULT_INFERENCE_TIMEOUT_SECONDS,
) -> dict[str, Any] | None:
    """Call the upstream OpenAI-compatible model gateway and return parsed JSON object.

    Returns None on any network error, timeout, non-200 status, or JSON parse failure.
    """
    hot_cfg = get_model_config_manager().get_config()
    upstream_base = hot_cfg.upstream_url or os.environ.get("IOAP_MODEL_GATEWAY_UPSTREAM_URL", "")
    upstream_key = hot_cfg.api_key or os.environ.get("IOAP_MODEL_GATEWAY_API_KEY", "")
    upstream_model = hot_cfg.model_name or os.environ.get(
        "IOAP_MODEL_GATEWAY_MODEL_NAME", "deepseek-v4-pro-0813"
    )
    effective_timeout = hot_cfg.timeout_seconds or timeout_seconds

    if not upstream_base or not upstream_key or upstream_key in {"disabled", ""}:
        return None

    try:
        with httpx.Client(base_url=upstream_base, timeout=effective_timeout) as client:
            resp = client.post(
                "/chat/completions",
                headers={"Authorization": f"Bearer {upstream_key}"},
                json={
                    "model": upstream_model,
                    "messages": [
                        {"role": "system", "content": system_instruction},
                        {"role": "user", "content": user_content},
                    ],
                    "response_format": {"type": "json_object"},
                    "temperature": 0.0,
                    "max_tokens": max_tokens,
                },
            )
        if resp.status_code == 200:
            resp_data = resp.json()
            choice = resp_data.get("choices", [{}])[0]
            message = choice.get("message", {})
            raw_text = message.get("content") or ""
            if not raw_text and "reasoning" in message:
                raw_text = str(message.get("reasoning"))
            if not raw_text and "reasoning_content" in message:
                raw_text = str(message.get("reasoning_content"))

            if raw_text:
                clean_text = raw_text.strip()
                if clean_text.startswith("```json"):
                    clean_text = clean_text[7:]
                if clean_text.startswith("```"):
                    clean_text = clean_text[3:]
                if clean_text.endswith("```"):
                    clean_text = clean_text[:-3]
                clean_text = clean_text.strip()
                return json.loads(clean_text)
    except Exception:
        return None

    return None
