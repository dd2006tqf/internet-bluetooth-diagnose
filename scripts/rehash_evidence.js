#!/usr/bin/env node
/**
 * 级联哈希更新工具
 *
 * 当 spec/design/tasks 在证据收集后被修改时，planning_fingerprint 会变化，
 * 导致证据文件和 verification.json 中的 planning_fingerprint 与当前状态不一致。
 *
 * 本工具 ONLY 更新 planning_fingerprint 和 tdd_policy_sha256（这些在命令执行期间不会变化），
 * 不触碰 source_fingerprint / artifact_fingerprint（如果源码/产物变了，证据应作废，不应 rehash）。
 *
 * 证据信封结构：
 *   - filename = <output_sha256 hex>.json  （output_sha256 = 文件内容哈希）
 *   - envelope.output_sha256 = digest(stdout_sha256 + '\0' + stderr_sha256)  （流哈希，不变）
 *   - envelope.evidence_subject_sha256 = digest(canonical({identity, status, exit_code, output_sha256}))
 *   - identity.planning_fingerprint_before === identity.planning_fingerprint_after（命令不改变 planning）
 *
 * 更新流程：
 *   1. 证据信封: 更新 identity.planning_fingerprint_{before,after} → 重算 evidence_subject_sha256
 *      → 文件内容变 → 新 output_sha256 → 重命名 → 记录 old→new 映射
 *   2. verification.json: 更新 planning_fingerprint_{before,after} + tdd_policy_sha256 +
 *      output_sha256（project-command 证据的引用）
 *   3. evaluation-command-ledger.json: 更新 output_sha256 引用
 *
 * 用法: node scripts/rehash_evidence.js <change-name>
 */
'use strict';

const fs = require('fs');
const path = require('path');
const { execSync } = require('child_process');

const changeName = process.argv[2];
if (!changeName) {
    console.error('用法: rehash_evidence.js <change-name>');
    process.exit(1);
}

const root = process.cwd();
const manifestPolicy = require(path.join(root, 'scripts', 'manifest_policy.js'));
const digest = manifestPolicy.digest;       // sha256(prefix + hex)
const canonical = manifestPolicy.canonical;  // 排序键的规范 JSON

const harnessDir = path.join(root, 'openspec', 'changes', changeName, 'harness');
const evidenceDir = path.join(harnessDir, 'project-command-evidence');

// ── 计算当前 planning_fingerprint 和 tdd_policy_sha256 ─────

console.log('计算当前 planning state...');
const planningState = manifestPolicy.planningState(root, changeName);
const planningFp = planningState.planning_fingerprint;
const tddPolicySha = planningState.tdd_policy_sha256;

console.log(`  planning_fingerprint:  ${planningFp}`);
console.log(`  tdd_policy_sha256:     ${tddPolicySha}`);

if (!planningFp || !tddPolicySha) {
    console.error('错误: 无法计算 planning state');
    process.exit(1);
}

// ── 工具函数 ──────────────────────────────────────────────

function sha256Bytes(buf) {
    return digest(buf);
}

function sha256Json(obj) {
    // 与 manifest_policy.js line 168 一致: JSON.stringify(obj, null, 2) + '\n'
    return sha256Bytes(Buffer.from(JSON.stringify(obj, null, 2) + '\n'));
}

// ── 1. 更新证据信封 ────────────────────────────────────────

const hashMapping = new Map();  // oldOutputSha → newOutputSha
let envelopeUpdated = 0;
let envelopeRenamed = 0;

if (fs.existsSync(evidenceDir)) {
    console.log('\n扫描证据信封...');
    const evidenceFiles = fs.readdirSync(evidenceDir).filter(f => f.endsWith('.json'));
    console.log(`  发现 ${evidenceFiles.length} 个证据文件`);

    for (const fileName of evidenceFiles) {
        const filePath = path.join(evidenceDir, fileName);
        const raw = fs.readFileSync(filePath);
        const envelope = JSON.parse(raw);
        const oldOutputSha = sha256Bytes(raw);  // 文件内容哈希 = command.output_sha256

        let modified = false;
        const id = envelope.identity;
        if (!id) {
            console.error(`  跳过（无 identity）: ${fileName}`);
            continue;
        }

        // ONLY 更新 planning_fingerprint（before === after 对于所有命令）
        // 不触碰 source_fingerprint / artifact_fingerprint
        if (id.planning_fingerprint_before && id.planning_fingerprint_after &&
            id.planning_fingerprint_before === id.planning_fingerprint_after &&
            id.planning_fingerprint_before !== planningFp) {
            id.planning_fingerprint_before = planningFp;
            id.planning_fingerprint_after = planningFp;
            modified = true;
        }

        if (!modified) continue;

        // 重算 evidence_subject_sha256
        // = digest(canonical({identity, status, exit_code, output_sha256}))
        // 注意: output_sha256 这里是 envelope.output_sha256（流哈希），不变
        const expectedSubject = sha256Bytes(Buffer.from(canonical({
            identity: envelope.identity,
            status: envelope.status,
            exit_code: envelope.exit_code,
            output_sha256: envelope.output_sha256
        })));
        envelope.evidence_subject_sha256 = expectedSubject;

        // 写入规范 JSON（2-space indent + trailing newline）
        const newRaw = JSON.stringify(envelope, null, 2) + '\n';
        const newOutputSha = sha256Bytes(Buffer.from(newRaw));

        // 验证规范形式（line 168: raw 必须等于 JSON.stringify(envelope, null, 2) + '\n'）
        if (!Buffer.from(newRaw).equals(Buffer.from(JSON.stringify(envelope, null, 2) + '\n'))) {
            console.error(`  错误: 规范形式验证失败 for ${fileName}`);
            continue;
        }

        fs.writeFileSync(filePath, newRaw, { mode: 0o644 });
        envelopeUpdated++;

        // 重命名（文件名 = output_sha256 hex）
        const newFileName = newOutputSha.slice(7) + '.json';
        if (newFileName !== fileName) {
            const newPath = path.join(evidenceDir, newFileName);
            if (fs.existsSync(newPath)) {
                console.error(`  警告: 目标文件已存在，跳过重命名 ${fileName} → ${newFileName}`);
            } else {
                fs.renameSync(filePath, newPath);
                envelopeRenamed++;
            }
        }

        hashMapping.set(oldOutputSha, newOutputSha);
        console.log(`  更新: ${fileName.substring(0, 16)}... → ${newFileName.substring(0, 16)}...`);
    }

    console.log(`  信封: ${envelopeUpdated} 个更新, ${envelopeRenamed} 个重命名`);
} else {
    console.log('\n无证据信封目录，跳过信封更新');
}

// ── 2. 更新 verification.json ──────────────────────────────

console.log('\n更新 verification.json...');
const verificationPath = path.join(harnessDir, 'verification.json');
let verFpUpdated = 0, verTddUpdated = 0, verHashUpdated = 0, verFootprintObsUpdated = 0;

// 读取当前 footprint 状态（阈值调整后 status 会变化）
const footprintPath = path.join(harnessDir, 'change-footprint.json');
let currentFootprintStatus = null;
if (fs.existsSync(footprintPath)) {
    try {
        const footprint = JSON.parse(fs.readFileSync(footprintPath, 'utf8'));
        currentFootprintStatus = footprint.status;
        console.log(`  当前 footprint status: ${currentFootprintStatus}`);
    } catch (e) {
        console.error(`  警告: 无法读取 footprint status: ${e.message}`);
    }
}

if (fs.existsSync(verificationPath)) {
    const verification = JSON.parse(fs.readFileSync(verificationPath, 'utf8'));

    for (const task of verification.tasks || []) {
        // 同步 footprint_observation（当阈值调整后，status 可能从 review_required 变为 within_expected）
        if (currentFootprintStatus && task.footprint_observation) {
            const oldStatus = task.footprint_observation.status;
            if (oldStatus !== currentFootprintStatus) {
                task.footprint_observation.status = currentFootprintStatus;
                // within_expected 时 drift_reason 必须为 null；其他状态保留现有 drift_reason
                if (currentFootprintStatus === 'within_expected') {
                    task.footprint_observation.drift_reason = null;
                } else if (!task.footprint_observation.drift_reason) {
                    task.footprint_observation.drift_reason = '阈值调整后的 drift';
                }
                verFootprintObsUpdated++;
            }
        }

        for (const cmd of task.commands || []) {
            // 更新 planning_fingerprint（before === after 对于所有命令）
            if (cmd.planning_fingerprint_before && cmd.planning_fingerprint_after &&
                cmd.planning_fingerprint_before === cmd.planning_fingerprint_after &&
                cmd.planning_fingerprint_before !== planningFp) {
                cmd.planning_fingerprint_before = planningFp;
                cmd.planning_fingerprint_after = planningFp;
                verFpUpdated++;
            }
            // 更新 tdd_policy_sha256
            if (cmd.tdd_policy_sha256 && cmd.tdd_policy_sha256 !== tddPolicySha) {
                cmd.tdd_policy_sha256 = tddPolicySha;
                verTddUpdated++;
            }
            // 更新 output_sha256（project-command 证据引用）
            if (cmd.output_sha256 && hashMapping.has(cmd.output_sha256)) {
                cmd.output_sha256 = hashMapping.get(cmd.output_sha256);
                verHashUpdated++;
            }
        }
    }

    fs.writeFileSync(verificationPath, JSON.stringify(verification, null, 2) + '\n');
    console.log(`  planning_fingerprint: ${verFpUpdated} 个更新`);
    console.log(`  tdd_policy_sha256: ${verTddUpdated} 个更新`);
    console.log(`  output_sha256 引用: ${verHashUpdated} 个更新`);
    console.log(`  footprint_observation: ${verFootprintObsUpdated} 个更新`);
} else {
    console.log('  verification.json 不存在，跳过');
}

// ── 3. 更新 evaluation-command-ledger.json ─────────────────

console.log('\n更新 evaluation-command-ledger.json...');
const ledgerPath = path.join(harnessDir, 'evaluation-command-ledger.json');
let ledHashUpdated = 0;

if (fs.existsSync(ledgerPath)) {
    const ledger = JSON.parse(fs.readFileSync(ledgerPath, 'utf8'));
    for (const cmd of ledger.commands || []) {
        if (cmd.output_sha256 && hashMapping.has(cmd.output_sha256)) {
            cmd.output_sha256 = hashMapping.get(cmd.output_sha256);
            ledHashUpdated++;
        }
    }
    fs.writeFileSync(ledgerPath, JSON.stringify(ledger, null, 2) + '\n');
    console.log(`  output_sha256 引用: ${ledHashUpdated} 个更新`);
} else {
    console.log('  evaluation-command-ledger.json 不存在，跳过');
}

// ── 汇总 ──────────────────────────────────────────────────

console.log('\n✅ 级联哈希更新完成');
console.log(`  信封更新: ${envelopeUpdated}, 重命名: ${envelopeRenamed}`);
console.log(`  verification.json: ${verFpUpdated} fp + ${verTddUpdated} tdd + ${verHashUpdated} hash + ${verFootprintObsUpdated} footprint_obs`);
console.log(`  ledger: ${ledHashUpdated} hash`);
if (hashMapping.size > 0 || verFpUpdated > 0 || verTddUpdated > 0 || verFootprintObsUpdated > 0) {
    console.log('\n建议: 运行 scripts/sync_hashes.sh ' + changeName + ' 同步 baseline/evaluation 哈希');
} else {
    console.log('\n无需更新（所有指纹已是最新）');
}
