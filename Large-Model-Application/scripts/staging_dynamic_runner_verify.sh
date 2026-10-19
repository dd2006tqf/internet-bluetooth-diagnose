#!/usr/bin/env bash
set -euo pipefail

case "${1:-}" in
  static)
    .venv/bin/python -B -m ruff check \
      src/industrial_ops_agent/staging_acceptance/cli.py \
      src/industrial_ops_agent/staging_acceptance/runner_contracts.py \
      src/industrial_ops_agent/staging_acceptance/runner.py
    command -v mypy >/dev/null 2>&1 && mypy --strict \
      --cache-dir .mypy_cache \
      src/industrial_ops_agent/staging_acceptance/cli.py \
      src/industrial_ops_agent/staging_acceptance/runner_contracts.py \
      src/industrial_ops_agent/staging_acceptance/runner.py
    echo STAGING_DYNAMIC_RUNNER_STATIC_OK
    ;;
  *)
    echo "usage: $0 {static}" >&2
    exit 2
    ;;
esac
