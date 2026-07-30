#!/usr/bin/env bash
# 生成 evaluation.json 骨架：从 baseline/ledger 自动填充机械字段，评审字段留 TODO 占位
# 用法: scripts/evaluation_template.sh <change>
# 前提: 已运行 evaluator_check.sh --begin 和 --run 收集证据
set -euo pipefail
source "$(dirname "$0")/harness_lock.sh"

change=${1:?用法: evaluation_template.sh <change>}
harness_validate_change_id "$change"

dir="openspec/changes/$change/harness"
baseline="$dir/evaluation-baseline.json"
ledger="$dir/evaluation-command-ledger.json"
evaluation="$dir/evaluation.json"

# 检查必要文件
for f in "$baseline" "$ledger"; do
  [[ -f "$f" && ! -L "$f" ]] || { echo "[ERR] 缺失或不安全: $f" >&2; exit 6; }
done

# 检查 baseline 状态
status=$(node -p "JSON.parse(require('fs').readFileSync(process.argv[1])).status" "$baseline")
[[ "$status" == "in_progress" ]] || { echo "[ERR] baseline 状态为 '$status'，只有 in_progress 状态才能生成模板（先运行 evaluator_check.sh --begin）" >&2; exit 6; }

# 检查 evaluation.json 是否已存在
if [[ -f "$evaluation" && ! -L "$evaluation" ]]; then
  echo "[WARN] evaluation.json 已存在，将被覆盖" >&2
fi

# 检查 ledger 是否有命令
cmd_count=$(node -p "JSON.parse(require('fs').readFileSync(process.argv[1])).commands.length" "$ledger")
[[ "$cmd_count" -gt 0 ]] || { echo "[ERR] ledger 中没有命令，请先运行 evaluator_check.sh --run 收集证据" >&2; exit 6; }

# 生成 evaluation.json
node - "$change" "$baseline" "$ledger" "$evaluation" <<'NODE'
const fs=require('fs'),path=require('path'),cp=require('child_process');
const [change,baselineFile,ledgerFile,evaluationFile]=process.argv.slice(2);
const baseline=JSON.parse(fs.readFileSync(baselineFile));
const ledger=JSON.parse(fs.readFileSync(ledgerFile));
const manifest=require(process.cwd()+'/scripts/manifest_policy.js');

// ── 读取文件 ──────────────────────────────────────────────
const tasksText=fs.readFileSync(`openspec/changes/${change}/tasks.md`,'utf8');
const designText=fs.readFileSync(`openspec/changes/${change}/design.md`,'utf8');
const footprint=JSON.parse(fs.readFileSync(`openspec/changes/${change}/harness/change-footprint.json`,'utf8'));
const snapshot=JSON.parse(fs.readFileSync(`openspec/changes/${change}/harness/ai_snapshot.json`,'utf8'));

// ── 解析 tasks ────────────────────────────────────────────
const parsed=manifest.parseTddPolicy(designText,tasksText);
const tasks=[...parsed.tasks.values()];
const taskIds=tasks.map(t=>t.id);
const requirementRefs=tasks.flatMap(t=>t.refs);

// ── 解析 budget block (v2) ────────────────────────────────
const budgetMatch=designText.match(/<!-- autoai:implementation-economy:v2 -->\s*```json\s*\n([\s\S]*?)\n```\s*<!-- \/autoai:implementation-economy:v2 -->/);
const budget=budgetMatch?JSON.parse(budgetMatch[1]):{reuse_decisions:[],obsolete_items:[],exceptions:[],structural_allowances:{}};

// ── 计算 implementationPaths (dirty paths 减去 managed) ───
const base=snapshot.implementation_base_commit;
const dirty=[...new Set([
  ...cp.execFileSync('git',['diff','--name-only','-z',base,'--']).toString('utf8').split('\0'),
  ...cp.execFileSync('git',['ls-files','--others','--exclude-standard','-z']).toString('utf8').split('\0')
].filter(Boolean))];
const policy=manifest.loadManifest();
const implementationPaths=dirty.filter(p=>!policy.isManaged(p)).sort();

// ── 常量 ──────────────────────────────────────────────────
const integrated=baseline.schema_version===3;
const now=new Date().toISOString().replace(/\.\d{3}Z$/,'Z');
const commands=ledger.commands;
const allCommandIds=commands.map(c=>c.id);
const passCommandIds=commands.filter(c=>c.result==='Pass').map(c=>c.id);
const failCommandIds=commands.filter(c=>c.result!=='Pass').map(c=>c.id);
const reviewPaths=baseline.review_input.review_paths;
const footprintStatus=footprint.status;

// ── 检查：如果有 Fail 命令，verdict 不能是 Pass ────────────
const hasFail=failCommandIds.length>0;
const defaultVerdict=hasFail?'Fail':'Pass';

// ── 构建 change_review ────────────────────────────────────
const stageEvidence=hasFail?passCommandIds:allCommandIds;
const changeReview={
  schema_version:1,
  git_state_fingerprint:baseline.review_input.git_state_fingerprint,
  stages:[
    {
      name:'specification_compliance',
      started_at:baseline.started_at,
      completed_at:now,
      status:defaultVerdict,
      requirement_refs:requirementRefs,
      task_ids:taskIds,
      reviewed_paths:reviewPaths,
      dimensions:['requirements','scenarios','scope','contracts','traceability'],
      evidence_command_ids:stageEvidence,
      finding_ids:[],
      blocking_untested_ids:[],
      not_run_reason:null
    },
    {
      name:'code_quality',
      started_at:now,
      completed_at:now,
      status:defaultVerdict,
      requirement_refs:requirementRefs,
      task_ids:taskIds,
      reviewed_paths:reviewPaths,
      dimensions:['correctness','safety','regression_risk','reuse','complexity','test_quality','repository_impact'],
      evidence_command_ids:stageEvidence,
      finding_ids:[],
      blocking_untested_ids:[],
      not_run_reason:null
    }
  ],
  findings:[]
};

// ── 构建 implementation_economy ───────────────────────────
// evidence_command_ids: Pass assessment 只能引用 Pass command
const ecoEvidence=passCommandIds.length>0?passCommandIds:[];
const ecoEvidencePaths=ecoEvidence.length>0?[]:implementationPaths;

const implementationEconomy={
  footprint_status:footprintStatus,
  drift_explanation:footprintStatus==='within_expected'?null:{
    metric_keys:[],
    reason:'TODO: 填写 drift 原因（哪些指标超出 expected 但低于 review_at）',
    why_no_replan:'TODO: 填写为什么不需要重新规划'
  },
  classification_assessment:{
    result:defaultVerdict,
    reason:'TODO: 填写分类评估原因（哪些文件被修改、是否属于 production/test 等）',
    evidence_paths:implementationPaths,
    evidence_command_ids:ecoEvidence
  },
  repository_impact_assessment:{
    result:defaultVerdict,
    surfaces:[
      {surface:'product_targets',applicability:'applicable',result:defaultVerdict,reason:'TODO: 填写 product_targets 评估',evidence_paths:implementationPaths,evidence_command_ids:ecoEvidence,not_applicable_reason:null},
      {surface:'install',applicability:'not_applicable',result:null,reason:'TODO: 无 install 变更',evidence_paths:[],evidence_command_ids:[],not_applicable_reason:'TODO: 填写无 install 变更原因'},
      {surface:'package',applicability:'not_applicable',result:null,reason:'TODO: 无 package 变更',evidence_paths:[],evidence_command_ids:[],not_applicable_reason:'TODO: 填写无 package 变更原因'},
      {surface:'ci',applicability:'not_applicable',result:null,reason:'TODO: 无 CI 变更',evidence_paths:[],evidence_command_ids:[],not_applicable_reason:'TODO: 填写无 CI 变更原因'}
    ]
  },
  reuse_assessments:(budget.reuse_decisions||[]).map(r=>({
    id:r.id,result:defaultVerdict,reason:'TODO: 填写复用评估原因',evidence_paths:ecoEvidencePaths,evidence_command_ids:ecoEvidence
  })),
  structural_assessments:[],
  obsolete_item_assessments:(budget.obsolete_items||[]).map(o=>({
    id:o.id,result:defaultVerdict,reason:'TODO: 填写废弃评估原因',evidence_paths:ecoEvidencePaths,evidence_command_ids:ecoEvidence
  })),
  exception_assessments:(budget.exceptions||[]).map(e=>({
    id:e.id,result:defaultVerdict,reason:'TODO: 填写例外评估原因',evidence_paths:ecoEvidencePaths,evidence_command_ids:ecoEvidence
  })),
  result:defaultVerdict
};

// ── 构建 criteria ─────────────────────────────────────────
const criteria=[{
  id:'criterion-001',
  description:'TODO: 填写验收标准描述',
  requirement_refs:requirementRefs,
  task_ids:taskIds,
  status:defaultVerdict,
  evidence_command_ids:hasFail?failCommandIds:passCommandIds,
  blocking_untested_ids:[]
}];

// ── 构建 integration_completeness（仅集成模式）────────────
let integrationCompleteness;
if(integrated){
  integrationCompleteness={
    planning_block_sha256:baseline.integration_planning_block_sha256,
    report_sha256:baseline.integration_surface_report_sha256,
    discovery_identity_sha256:baseline.integration_discovery_identity_sha256,
    inventory_assessment:null,
    candidate_assessments:[],
    surface_assessments:[],
    orphan_surfaces:[],
    result:defaultVerdict
  };
}

// ── 组装 evaluation.json ──────────────────────────────────
const evaluation={
  schema_version:integrated?3:2,
  evaluation_id:baseline.evaluation_id,
  change_name:change,
  verdict:defaultVerdict,
  evaluation_started_at:baseline.started_at,
  evaluated_at:now,
  openspec_version:'1.6.0',
  evaluator_role:'independent',
  input_source_fingerprint:baseline.source_fingerprint,
  input_artifact_fingerprint:baseline.artifact_fingerprint,
  input_base_specs_fingerprint:baseline.base_specs_fingerprint,
  source_fingerprint:baseline.source_fingerprint,
  artifact_fingerprint:baseline.artifact_fingerprint,
  base_specs_fingerprint:baseline.base_specs_fingerprint,
  budget_block_sha256:baseline.budget_block_sha256,
  change_footprint_json_sha256:baseline.change_footprint_json_sha256,
  review_input:baseline.review_input,
  change_review:changeReview,
  implementation_economy:implementationEconomy,
  criteria:criteria,
  commands:commands,
  blocking_untested:[],
  residual_risks:[]
};
if(integrated)evaluation.integration_completeness=integrationCompleteness;

// ── 原子写入 ──────────────────────────────────────────────
const tmp=path.join(path.dirname(evaluationFile),`.evaluation-template-${process.pid}-${Date.now()}`);
fs.writeFileSync(tmp,JSON.stringify(evaluation,null,2)+'\n',{mode:0o644,flag:'wx'});
fs.renameSync(tmp,evaluationFile);

// ── 输出提示 ──────────────────────────────────────────────
console.log('✅ evaluation.json 模板已生成: '+evaluationFile);
console.log('');
console.log('📋 已自动填充的字段:');
console.log('  - evaluation_id, evaluation_started_at, evaluated_at');
console.log('  - source/artifact/base_specs_fingerprint');
console.log('  - budget_block_sha256, change_footprint_json_sha256');
console.log('  - review_input, commands');
console.log('  - change_review.stages (dimensions, requirement_refs, task_ids, reviewed_paths)');
console.log('');
console.log('✏️  需要手动检查/填写的字段（搜索 TODO）:');
console.log('  - verdict: '+defaultVerdict+(hasFail?' (有 Fail 命令，请确认)':''));
console.log('  - change_review.stages[].status: '+defaultVerdict);
console.log('  - implementation_economy 各 assessment 的 reason');
console.log('  - criteria[].description');
if(footprintStatus==='drift_warning'){
  console.log('  - implementation_economy.drift_explanation (footprint 为 drift_warning)');
}
if(integrated){
  console.log('  - integration_completeness.inventory_assessment');
  console.log('  - integration_completeness.candidate_assessments');
}
console.log('');
console.log('⚠️  注意:');
console.log('  - Pass assessment 只能引用 result=Pass 的命令');
console.log('  - 所有命令必须被引用（stages/criteria/assessments/integration）');
console.log('  - 完成后运行 evaluator_check.sh --finish 验证');
NODE
