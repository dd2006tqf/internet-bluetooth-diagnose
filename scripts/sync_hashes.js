#!/usr/bin/env node
/**
 * 自动同步 harness 中所有相关文件的哈希值
 * 解决级联哈希更新的手动操作问题
 */

const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const { execSync } = require('child_process');

const changeName = process.argv[2];
if (!changeName) {
    console.error('用法: sync_hashes.js <change-name>');
    process.exit(1);
}

const changeDir = `openspec/changes/${changeName}`;
const harnessDir = `${changeDir}/harness`;

// 检查必要文件是否存在
const requiredFiles = [
    `${changeDir}/design.md`,
    `${harnessDir}/ai_snapshot.json`,
    `${harnessDir}/change-footprint.json`,
    `${harnessDir}/evaluation-baseline.json`,
    `${harnessDir}/evaluation.json`,
    `${harnessDir}/evaluation-command-ledger.json`,
    `${harnessDir}/integration-surface-report.json`,
    `${harnessDir}/verification.json`
];

for (const file of requiredFiles) {
    if (!fs.existsSync(file)) {
        console.error(`错误: 文件不存在: ${file}`);
        process.exit(1);
    }
}

// 计算 SHA256 哈希
function sha256(content) {
    return 'sha256:' + crypto.createHash('sha256').update(content).digest('hex');
}

// 计算文件的 SHA256
function sha256File(filePath) {
    return sha256(fs.readFileSync(filePath));
}

// 计算 planning_fingerprint（与 manifest_policy.js 逻辑一致）
function calculatePlanningFingerprint(changeDir) {
    const required = ['.openspec.yaml', 'proposal.md', 'design.md', 'tasks.md'];
    const files = [];
    
    for (const rel of required) {
        const file = path.join(changeDir, rel);
        if (!fs.existsSync(file)) continue;
        
        const st = fs.lstatSync(file);
        if (!st.isFile() || st.isSymbolicLink()) continue;
        
        let content = fs.readFileSync(file);
        const mode = (st.mode & 0o777).toString(8);
        
        if (rel === 'tasks.md') {
            // 将所有 - [x] 或 - [X] 替换为 - [ ]
            const text = content.toString('utf8').replace(/^- \[[xX ]\] (\d+(?:\.\d+)*)/gm, '- [ ] $1');
            content = Buffer.from(text);
        }
        
        if (rel === 'design.md') {
            // 只提取 TDD Policy v1 块的内容
            const text = content.toString('utf8');
            const start = text.indexOf('<!-- autoai:tdd-policy:v1 -->');
            const end = text.indexOf('<!-- /autoai:tdd-policy:v1 -->');
            if (start >= 0 && end >= 0 && start < end) {
                content = Buffer.from(text.slice(start + '<!-- autoai:tdd-policy:v1 -->'.length, end));
            }
        }
        
        files.push({
            path: rel,
            mode: mode,
            sha256: sha256(content)
        });
    }
    
    // 收集 specs/ 目录下的所有 .md 文件
    const specsDir = path.join(changeDir, 'specs');
    if (fs.existsSync(specsDir) && fs.lstatSync(specsDir).isDirectory()) {
        const walk = (dir, rel) => {
            for (const name of fs.readdirSync(dir).sort()) {
                const p = path.join(dir, name);
                const r = rel + '/' + name;
                const st = fs.lstatSync(p);
                if (st.isSymbolicLink()) continue;
                if (st.isDirectory()) {
                    walk(p, r);
                } else if (st.isFile() && name.endsWith('.md')) {
                    const content = fs.readFileSync(p);
                    const mode = (st.mode & 0o777).toString(8);
                    files.push({
                        path: r,
                        mode: mode,
                        sha256: sha256(content)
                    });
                }
            }
        };
        walk(specsDir, 'specs');
    }
    
    // 排序并计算 canonical 哈希
    files.sort((a, b) => a.path.localeCompare(b.path));
    return sha256(JSON.stringify(files));
}

// 从 design.md 提取 JSON 块（支持 v1 和 v2）
function extractJsonBlock(content, blockType) {
    const regex = new RegExp(`<!-- autoai:${blockType}:v(\\d+) -->\\s*\\n\\s*\`\`\`json\\s*\\n([\\s\\S]*?)\\n\\s*\`\`\`\\s*\\n<!-- /autoai:${blockType}:v\\d+ -->`);
    const match = content.match(regex);
    if (!match) {
        throw new Error(`未找到 ${blockType} 块`);
    }
    return JSON.parse(match[2]);
}

// 读取所有文件
console.log('读取文件...');
const designContent = fs.readFileSync(`${changeDir}/design.md`, 'utf8');
const snapshot = JSON.parse(fs.readFileSync(`${harnessDir}/ai_snapshot.json`, 'utf8'));
const footprint = JSON.parse(fs.readFileSync(`${harnessDir}/change-footprint.json`, 'utf8'));
const baseline = JSON.parse(fs.readFileSync(`${harnessDir}/evaluation-baseline.json`, 'utf8'));
const evaluation = JSON.parse(fs.readFileSync(`${harnessDir}/evaluation.json`, 'utf8'));
const ledger = JSON.parse(fs.readFileSync(`${harnessDir}/evaluation-command-ledger.json`, 'utf8'));
const surfaceReport = JSON.parse(fs.readFileSync(`${harnessDir}/integration-surface-report.json`, 'utf8'));
const verification = JSON.parse(fs.readFileSync(`${harnessDir}/verification.json`, 'utf8'));

// 1. 计算 design.md 中各块的哈希
console.log('计算 design.md 块哈希...');
const tddPolicy = extractJsonBlock(designContent, 'tdd-policy');
const implEconomy = extractJsonBlock(designContent, 'implementation-economy');
const integrationCompleteness = extractJsonBlock(designContent, 'integration-completeness');

const tddPolicyHash = sha256(JSON.stringify(tddPolicy));
const implEconomyHash = sha256(JSON.stringify(implEconomy));
const integrationCompletenessHash = sha256(JSON.stringify(integrationCompleteness));
const planningFingerprint = calculatePlanningFingerprint(changeDir);

console.log(`  tdd_policy_sha256: ${tddPolicyHash}`);
console.log(`  budget_block_sha256: ${implEconomyHash}`);
console.log(`  integration_completeness_sha256: ${integrationCompletenessHash}`);
console.log(`  planning_fingerprint: ${planningFingerprint}`);

// 2. 更新 ai_snapshot.json
console.log('\n更新 ai_snapshot.json...');
snapshot.planned_tdd_policy_sha256 = tddPolicyHash;
snapshot.planned_change_fingerprint = planningFingerprint;
snapshot.planned_integration_completeness_sha256 = integrationCompletenessHash;

// 3. 重新生成 change-footprint.json
console.log('\n重新生成 change-footprint.json...');
const classification = implEconomy.classification;
const baseCommit = snapshot.implementation_base_commit;

try {
    const footprintOutput = execSync(
        `node scripts/change_scope.js ${baseCommit} '${JSON.stringify(classification)}'`,
        { encoding: 'utf8', stdio: ['pipe', 'pipe', 'pipe'] }
    );
    const newFootprint = JSON.parse(footprintOutput);
    
    // 更新 footprint 文件
    footprint.implementation_base_commit = baseCommit;
    footprint.source_fingerprint = newFootprint.source_fingerprint;
    footprint.budget_block_sha256 = implEconomyHash;
    footprint.status = newFootprint.status;
    footprint.production = newFootprint.production;
    footprint.tests = newFootprint.tests;
    footprint.project_support = newFootprint.project_support;
    footprint.generated = newFootprint.generated;
    footprint.vendor = newFootprint.vendor;
    footprint.binary = newFootprint.binary;
    footprint.structural_candidates = newFootprint.structural_candidates;
    footprint.unclassified_paths = newFootprint.unclassified_paths;
    footprint.classification_overlaps = newFootprint.classification_overlaps;
    
    // 计算新的 footprint 哈希
    const footprintHash = sha256(JSON.stringify(footprint, null, 2));
    console.log(`  change_footprint_json_sha256: ${footprintHash}`);
    
    // 4. 更新 integration-surface-report.json
    console.log('\n更新 integration-surface-report.json...');
    surfaceReport.change_footprint_json_sha256 = footprintHash;
    surfaceReport.planning_block_sha256 = integrationCompletenessHash;
    
    // 计算 discovery identity
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
    
    // 5. 更新 evaluation-baseline.json
    console.log('\n更新 evaluation-baseline.json...');
    baseline.source_fingerprint = newFootprint.source_fingerprint;
    baseline.budget_block_sha256 = implEconomyHash;
    baseline.change_footprint_json_sha256 = footprintHash;
    baseline.integration_planning_block_sha256 = integrationCompletenessHash;
    baseline.integration_surface_report_sha256 = surfaceReportHash;
    baseline.integration_discovery_identity_sha256 = discoveryIdentityHash;
    baseline.verification_json_sha256 = sha256File(`${harnessDir}/verification.json`);
    
    // 6. 更新 evaluation.json
    console.log('\n更新 evaluation.json...');
    evaluation.source_fingerprint = newFootprint.source_fingerprint;
    evaluation.budget_block_sha256 = implEconomyHash;
    evaluation.change_footprint_json_sha256 = footprintHash;
    evaluation.integration_completeness.planning_block_sha256 = integrationCompletenessHash;
    evaluation.integration_completeness.report_sha256 = surfaceReportHash;
    evaluation.integration_completeness.discovery_identity_sha256 = discoveryIdentityHash;
    
    // 7. 更新 evaluation-command-ledger.json
    console.log('\n更新 evaluation-command-ledger.json...');
    // ledger 的哈希不需要更新，因为命令本身没有改变
    
    // 8. 更新 verification.json 中的哈希
    console.log('\n更新 verification.json...');
    // verification 中的命令哈希需要与 ledger 保持一致
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
    
    // 写回所有文件
    console.log('\n写入文件...');
    fs.writeFileSync(`${harnessDir}/ai_snapshot.json`, JSON.stringify(snapshot, null, 2) + '\n');
    fs.writeFileSync(`${harnessDir}/change-footprint.json`, JSON.stringify(footprint, null, 2) + '\n');
    fs.writeFileSync(`${harnessDir}/integration-surface-report.json`, JSON.stringify(surfaceReport, null, 2) + '\n');
    fs.writeFileSync(`${harnessDir}/evaluation-baseline.json`, JSON.stringify(baseline, null, 2) + '\n');
    fs.writeFileSync(`${harnessDir}/evaluation.json`, JSON.stringify(evaluation, null, 2) + '\n');
    fs.writeFileSync(`${harnessDir}/verification.json`, JSON.stringify(verification, null, 2) + '\n');
    
    console.log('\n✅ 所有哈希已同步');
    
} catch (error) {
    console.error('错误: 生成 footprint 失败');
    console.error(error.message);
    if (error.stderr) {
        console.error('stderr:', error.stderr.toString());
    }
    process.exit(1);
}
