# RCA prompt

Read the complete failure and reproduce it before editing. Compare a working repository pattern, state one causal hypothesis, and run one minimal single-variable experiment. Preserve the reproducer, make the smallest direct root-cause fix, run focused and related regression evidence, and record a reusable Trigger/Check only when justified. Keep change-specific evidence under the active change harness. A missing planned consumer or broken dispatch returns to Generator; an unplanned surface or wrong requirement/task/classification/scope returns to Planner. Do not repair either by adding another speculative interface.

Track validated attempts for the same issue. After three unsuccessful fixes, stop adding patches and return to Planner/user for architecture and assumption review. Any contract, dependency, target, structural allowance or budget change requires revised OpenSpec planning and human approval.

## Recording the RCA

Use `scripts/rca_new.sh [change]` to append the entry rather than editing `openspec/changes/<change>/harness/defect-rca.md` by hand. The script takes the harness lock, creates the file if absent, appends the dated template (Symptom / Reproducer / Root cause / Direct fix / Regression evidence / Reusable Trigger-Check rule), and detects concurrent modification by comparing the file hash before and after. Hand-editing bypasses all three guarantees and has produced lost entries.
