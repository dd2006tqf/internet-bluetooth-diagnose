.PHONY: m1-init m1-up m1-verify m1-status m1-logs m1-down m1-clean static web-check \
	verify-staging-attestation verify-staging-runner verify-staging-assurance \
	verify-inference-pool verify-kserve-autoscale

m1-init:
	./scripts/dev_lite.sh init

m1-up:
	./scripts/dev_lite.sh up

m1-verify:
	./scripts/m1_verify.sh compose-lite

# O12：给 5 个此前零调用的验证脚本挂入 Makefile 目标，成为常态门禁
verify-staging-attestation:
	./scripts/staging_attestation_verify.sh static-core

verify-staging-runner:
	./scripts/staging_dynamic_runner_verify.sh static

verify-staging-assurance:
	./scripts/staging_production_assurance_verify.sh static-backend

verify-inference-pool:
	./scripts/inference_pool_routing_verify.sh backend

verify-kserve-autoscale:
	./scripts/kserve_gpu_autoscaling_verify.sh backend

m1-status:
	./scripts/dev_lite.sh status

m1-logs:
	./scripts/dev_lite.sh logs

m1-down:
	./scripts/dev_lite.sh down

m1-clean:
	./scripts/dev_lite.sh clean

static:
	.venv/bin/ruff check src
	.venv/bin/mypy src

web-check:
	web/node_modules/.bin/pnpm --dir web typecheck
	web/node_modules/.bin/pnpm --dir web build
