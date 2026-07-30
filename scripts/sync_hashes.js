#!/usr/bin/env node
/**
 * 自动同步 harness 中所有相关文件的哈希值
 * 支持两种模式：
 *   - pre-evaluation：evaluation-baseline.json 不存在时，只更新 ai_snapshot.json + change-footprint.json
 *   - full：所有 evaluation 文件都存在时，级联更新所有哈希
 * 
 * 哈希计算统一使用 manifest_policy.js 的函数，确保一致性
 */

const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const { execSync } = require('child_process');
const manifestPolicy = require('./manifest_policy.js');

const changeName = process.argv[2];
if (!changeName) {
    console.error('用法: sync_hashes.js <change-name>');
    process.exit(1);
}

const changeDir = `openspec/changes/${changeName}`;
const harnessDir = `${changeDir}/harness`;

// ── 工具函数（统一使用 manifest_policy.js 的实现）─────────────

// 使用 manifest_policy.js 的 digest 函数（等同于 sha256）
const sha256 = manifestPolicy.digest;

// 使用 manifest_policy.js 的 canonical 函数
const canonical = manifestPolicy.canonical;

function sha256File(filePath) {
    return sha256(fs.readFileSync(filePath));
}

// 从 design.md 提取 JSON 块
function extractJsonBlock(content, blockType) {
    const regex = new RegExp(`<!-- autoai:${blockType}:v(\\d+) -->\\s*\\n\\s*\`\`\`json\\s*\\n([\\s\\S]*?)\\n\\s*\`\`\`\\s*\\n<!-- /autoai:${blockType}:v\\d+ -->`);
    const match = content.match(regex);
    if (!match) {
        throw new Error(`未找到 ${blockType} 块`);
    }
    return JSON.parse(match[2]);
}

// ── 检查必要文件 ──────────────────────────────────────────

const requiredFiles = [
    `${changeDir}/design.md`,
    `${harnessDir}/ai_snapshot.json`
];

for (const file of requiredFiles) {
    if (!fs.existsSync(file)) {
        console.error(`错误: 文件不存在: ${file}`);
        process.exit(1);
    }
}

// ── 检查可选文件（Evaluation 阶段才会创建） ──────────────

const optionalPaths = {
    footprint: `${harnessDir}/change-footprint.json`,
    baseline: `${harnessDir}/evaluation-baseline.json`,
    evaluation: `${harnessDir}/evaluation.json`,
    ledger: `${harnessDir}/evaluation-command-ledger.json`,
    surfaceReport: `${harnessDir}/integration-surface-report.json`,
    verification: `${harnessDir}/verification.json`
};

const exists = {};
for (const [key, p] of Object.entries(optionalPaths)) {
    exists[key] = fs.existsSync(p);
}

// pre-evaluation 模式：evaluation-baseline.json 不存在
const isPreEvaluation = !exists.baseline;
if (isPreEvaluation) {
    console.log('检测到 pre-evaluation 模式，将只更新 ai_snapshot.json 和 change-footprint.json');
}

// ── 读取文件 ──────────────────────────────────────────────

console.log('读取文件...');
const designContent = fs.readFileSync(`${changeDir}/design.md`, 'utf8');
const snapshot = JSON.parse(fs.readFileSync(`${harnessDir}/ai_snapshot.json`, 'utf8'));

// ── 自动填充缺失的 planning 字段 ──────────────────────────

// 自动填充 planning_approved_at（若为 null，设为当前 UTC 时间）
if (!snapshot.planning_approved_at) {
    snapshot.planning_approved_at = new Date().toISOString().replace(/\.\d{3}Z$/, 'Z');
    console.log(`自动填充 planning_approved_at: ${snapshot.planning_approved_at}`);
}

// 自动填充 planned_base_specs_fingerprint（若为 null，通过 source_fingerprint.sh 计算）
if (!snapshot.planned_base_specs_fingerprint) {
    try {
        const baseSpecsFp = execSync(
            `scripts/source_fingerprint.sh --kind base-specs --change ${changeName}`,
            { encoding: 'utf8', stdio: ['pipe', 'pipe', 'pipe'] }
        ).trim();
        if (/^sha256:[0-9a-f]{64}$/.test(baseSpecsFp)) {
            snapshot.planned_base_specs_fingerprint = baseSpecsFp;
            console.log(`自动填充 planned_base_specs_fingerprint: ${baseSpecsFp}`);
        }
    } catch (e) {
        console.error('警告: 无法计算 planned_base_specs_fingerprint:', e.message);
    }
}

const baseline = exists.baseline
    ? JSON.parse(fs.readFileSync(optionalPaths.baseline, 'utf8'))
    : null;
const evaluation = exists.evaluation
    ? JSON.parse(fs.readFileSync(optionalPaths.evaluation, 'utf8'))
    : null;
const ledger = exists.ledger
    ? JSON.parse(fs.readFileSync(optionalPaths.ledger, 'utf8'))
    : null;
const surfaceReport = exists.surfaceReport
    ? JSON.parse(fs.readFileSync(optionalPaths.surfaceReport, 'utf8'))
    : null;
const verification = exists.verification
    ? JSON.parse(fs.readFileSync(optionalPaths.verification, 'utf8'))
    : null;

// ── 1. 计算 design.md 中各块的哈希 ────────────────────────

console.log('计算 design.md 块哈希...');
const tddPolicy = extractJsonBlock(designContent, 'tdd-policy');
const implEconomy = extractJsonBlock(designContent, 'implementation-economy');

// 使用 manifest_policy.js 的 planningState 计算所有规划哈希
// 这确保了与 manifest_policy.js 的计算方式完全一致
const planningState = manifestPolicy.planningState(process.cwd(), changeName);

const tddPolicyHash = sha256(Buffer.from(canonical(tddPolicy)));
const implEconomyHash = sha256(Buffer.from(canonical(implEconomy)));
const integrationCompletenessHash = planningState.integration_completeness_sha256;
const planningFingerprint = planningState.planning_fingerprint;

console.log(`  tdd_policy_sha256: ${tddPolicyHash}`);
console.log(`  budget_block_sha256: ${implEconomyHash}`);
console.log(`  integration_completeness_sha256: ${integrationCompletenessHash}`);
console.log(`  planning_fingerprint: ${planningFingerprint}`);

// ── 2. 更新 ai_snapshot.json ──────────────────────────────

console.log('\n更新 ai_snapshot.json...');
snapshot.planned_tdd_policy_sha256 = tddPolicyHash;
snapshot.planned_change_fingerprint = planningFingerprint;
snapshot.planned_integration_completeness_sha256 = integrationCompletenessHash;

// ── 3. 重新生成 change-footprint.json ─────────────────────

console.log('\n重新生成 change-footprint.json（通过 change_footprint.sh）...');
try {
    execSync(`bash scripts/change_footprint.sh ${changeName} --json`, {
        encoding: 'utf8', stdio: ['pipe', 'pipe', 'pipe']
    });
} catch (error) {
    console.error('错误: change_footprint.sh 生成 footprint 失败');
    console.error(error.message);
    if (error.stderr) {
        console.error('stderr:', error.stderr.toString());
    }
    process.exit(1);
}

// 读取由 change_footprint.sh 生成的 footprint
const footprintObj = JSON.parse(fs.readFileSync(optionalPaths.footprint, 'utf8'));
const footprintHash = sha256(JSON.stringify(footprintObj, null, 2));
console.log(`  change_footprint_json_sha256: ${footprintHash}`);

// 写回 ai_snapshot.json + change-footprint.json
fs.writeFileSync(`${harnessDir}/ai_snapshot.json`, JSON.stringify(snapshot, null, 2) + '\n');
fs.writeFileSync(optionalPaths.footprint, JSON.stringify(footprintObj, null, 2) + '\n');

// ── pre-evaluation 模式到此结束 ────────────────────────────

if (isPreEvaluation) {
    console.log('\n✅ Pre-evaluation 同步完成（已更新 ai_snapshot.json 和 change-footprint.json）');
    console.log('现在可以运行 evaluator_check.sh --begin 开始 Evaluation 阶段');
    process.exit(0);
}

// ── 4. full 模式：级联更新 evaluation 相关文件 ────────────

// 4a. integration-surface-report.json
console.log('\n更新 integration-surface-report.json...');
surfaceReport.change_footprint_json_sha256 = footprintHash;
surfaceReport.planning_block_sha256 = integrationCompletenessHash;

const discoveryIdentityHash = sha256(JSON.stringify({
    discovery_mode: surfaceReport.discovery_mode,
    source_fingerprint: surfaceReport.source_fingerprint,
    changed_production_paths: surfaceReport.changed_production_paths,
    structural_candidates: surfaceReport.structural_candidates
}));
surfaceReport.discovery_identity_sha256 = discoveryIdentityHash;

const surfaceReportHash = sha256(JSON.stringify(surfaceReport, null, 2));
console.log(`  integration_surface_report_sha256: ${surfaceReportHash}`);
console.log(`  discovery_identity_sha256: ${discoveryIdentityHash}`);

// 4b. evaluation-baseline.json
console.log('\n更新 evaluation-baseline.json...');
baseline.source_fingerprint = footprintObj.source_fingerprint;
baseline.budget_block_sha256 = implEconomyHash;
baseline.change_footprint_json_sha256 = footprintHash;
baseline.integration_planning_block_sha256 = integrationCompletenessHash;
baseline.integration_surface_report_sha256 = surfaceReportHash;
baseline.integration_discovery_identity_sha256 = discoveryIdentityHash;
baseline.verification_json_sha256 = sha256File(optionalPaths.verification);

// 4c. evaluation.json
console.log('\n更新 evaluation.json...');
evaluation.source_fingerprint = footprintObj.source_fingerprint;
evaluation.budget_block_sha256 = implEconomyHash;
evaluation.change_footprint_json_sha256 = footprintHash;
evaluation.integration_completeness.planning_block_sha256 = integrationCompletenessHash;
evaluation.integration_completeness.report_sha256 = surfaceReportHash;
evaluation.integration_completeness.discovery_identity_sha256 = discoveryIdentityHash;

// 4d. verification.json（同步 ledger 中的命令输出哈希）
console.log('\n更新 verification.json...');
if (verification.tasks && ledger.commands) {
    const commandMap = new Map();
    for (const cmd of ledger.commands) {
        commandMap.set(cmd.id, cmd.output_sha256);
    }
    for (const task of verification.tasks) {
        if (task.commands) {
            for (const cmd of task.commands) {
                if (commandMap.has(cmd.id)) {
                    cmd.output_sha256 = commandMap.get(cmd.id);
                }
            }
        }
    }
}

// ── 写回所有 evaluation 文件 ──────────────────────────────

console.log('\n写入文件...');
fs.writeFileSync(optionalPaths.surfaceReport, JSON.stringify(surfaceReport, null, 2) + '\n');
fs.writeFileSync(optionalPaths.baseline, JSON.stringify(baseline, null, 2) + '\n');
fs.writeFileSync(optionalPaths.evaluation, JSON.stringify(evaluation, null, 2) + '\n');
fs.writeFileSync(optionalPaths.verification, JSON.stringify(verification, null, 2) + '\n');

console.log('\n✅ 所有哈希已同步（full 模式）');
