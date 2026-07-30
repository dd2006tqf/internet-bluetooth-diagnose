#!/usr/bin/env bash
# audit_summary.sh — 生成人类可读的 TDD 审计汇总
# 用法: bash scripts/audit_summary.sh <change-name>
set -euo pipefail
CHANGE="${1:-}"
[ -z "$CHANGE" ] && { echo "用法: $0 <change-name>" >&2; exit 1; }
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BASE="$ROOT/openspec/changes/$CHANGE"
[ -d "$BASE" ] || { echo "[ERR] 变更目录不存在: $BASE" >&2; exit 1; }
HARNESS="$BASE/harness"
VJSON="$HARNESS/verification.json"
[ -f "$VJSON" ] || { echo "[ERR] verification.json 不存在: $VJSON" >&2; exit 1; }
EVIDENCE_DIR="$HARNESS/project-command-evidence"
EVIDENCE_COUNT=0
[ -d "$EVIDENCE_DIR" ] && EVIDENCE_COUNT=$(find "$EVIDENCE_DIR" -name '*.json' | wc -l)
OUT="$HARNESS/audit-summary.md"

node -e "
const fs=require('fs');
const v=JSON.parse(fs.readFileSync('$VJSON','utf8'));
const lines=[];
lines.push('# TDD 审计汇总 — $CHANGE');
lines.push('');
lines.push('生成时间: '+new Date().toISOString());
lines.push('');
lines.push('## 概览');
lines.push('');
lines.push('- schema_version: '+v.schema_version);
lines.push('- change_name: '+v.change_name);
lines.push('- 任务总数: '+(v.tasks?v.tasks.length:0));
lines.push('- 证据文件数: $EVIDENCE_COUNT');
lines.push('');
lines.push('## 各任务 TDD 闭环');
lines.push('');
lines.push('| Task | Phase | Kind | Result | Cycle |');
lines.push('|------|-------|------|--------|-------|');
for(const t of (v.tasks||[])){
  for(const c of (t.commands||[])){
    lines.push('| '+t.task_id+' | '+c.phase+' | '+c.kind+' | '+c.result+' | '+(c.cycle_id||'-')+' |');
  }
}
lines.push('');
lines.push('## 闭环状态');
lines.push('');
for(const t of (v.tasks||[])){
  const cmds=t.commands||[];
  const reds=cmds.filter(c=>c.phase==='RED');
  const greens=cmds.filter(c=>c.phase==='GREEN');
  const regressions=cmds.filter(c=>c.phase==='REGRESSION');
  const alts=cmds.filter(c=>c.phase==='ALTERNATIVE');
  const closed=reds.length>0&&greens.length>0&&regressions.length>0;
  const status=closed?'✅ 闭环':alts.length>0?'⚠️ 替代验证':'❌ 未闭环';
  lines.push('- **Task '+t.task_id+'**: '+status+' (RED='+reds.length+', GREEN='+greens.length+', REGRESSION='+regressions.length+', ALT='+alts.length+')');
}
lines.push('');
fs.writeFileSync('$OUT',lines.join('\n')+'\n');
console.log('已生成: $OUT');
"
