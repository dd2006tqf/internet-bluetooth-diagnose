---
name: executing-plans
description: Use when you have a written implementation plan to execute in a separate session with review checkpoints
---

# Executing Plans

## Overview

Load plan, review critically, execute all tasks, report when complete.

**Announce at start:** "I'm using the executing-plans skill to implement this plan."

**Adaptation note for this project:** When executing within an OpenSpec change, use harness scripts (task_verify.sh, evaluator_check.sh) for recording evidence. For this project, also verify via ctest and the specified build commands.

## The Process

### Step 1: Load and Review Plan
1. Ensure isolated workspace via using-git-worktrees skill
2. Read plan file
3. Review critically
4. If concerns: raise with human partner
5. Create todos for plan items

### Step 2: Execute Tasks
For each task:
1. Mark as in_progress
2. Follow each step exactly
3. Run verifications
4. For OpenSpec: record evidence via task_verify.sh
5. Mark as completed

### Step 3: Complete Development
- Use finishing-a-development-branch skill
- For OpenSpec changes: follow archive workflow

## When to STOP

- Hit a blocker (missing dependency, test fails)
- Plan has critical gaps
- Verification fails repeatedly
- For this project: unplanned product surfaces must return to Planner

## Remember
- Review plan critically first
- Follow plan steps exactly
- Don't skip verifications
- Stop when blocked, don't guess
- Never start on main/master without explicit consent
- For this project: consult AGENTS.md and harness scripts
