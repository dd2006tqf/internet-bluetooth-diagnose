# Debt scan prompt

Inspect the whole repository against `docs/ai/golden-principles.md`. Record small, evidenced debt items without modifying active OpenSpec planning or bundling unrelated fixes.

Start from `scripts/ai_debt_scan.sh`, which greps the C/C++/CMake surface for TODO/FIXME/HACK markers and prints the matching locations. It is a heuristic sweep, not a verdict: every candidate still needs a human-read rationale before it becomes a debt item, and the script deliberately edits nothing. Do not treat its output as an approved work queue.
