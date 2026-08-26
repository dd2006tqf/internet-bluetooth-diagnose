---
name: writing-plans
description: Use when you have a spec or requirements for a multi-step task, before touching code
---

# Writing Plans

## Overview

Write comprehensive implementation plans. Document everything: files to touch, code, testing. Give them as bite-sized tasks. DRY. YAGNI. TDD.

**Announce at start:** "I'm using the writing-plans skill to create the implementation plan."

**Adaptation note for this project:** For OpenSpec changes, align plans with the tasks.md format. This skill's task decomposition techniques complement the Planner role's task planning.

**Save plans to:** docs/superpowers/plans/YYYY-MM-DD-<feature-name>.md

## Scope Check

If spec covers multiple independent subsystems, break into separate plans.

## File Structure

Map out which files will be created/modified and what each is responsible for. Design units with clear boundaries. Files that change together should live together.

## Task Right-Sizing

A task is the smallest unit that carries its own test cycle. Each task ends with an independently testable deliverable.

## Plan Document Header

```markdown
# [Feature Name] Implementation Plan

> REQUIRED SUB-SKILL: Use subagent-driven-development or executing-plans

**Goal:** [One sentence]
**Architecture:** [2-3 sentences]
**Tech Stack:** [Key technologies]
**Spec:** [path to spec]

## Global Constraints
[project-wide requirements]

---
```

## Task Structure

```markdown
### Task N: [Component Name]

**Files:** Create/Modify/Test paths
**Interfaces:** Consumes/Produces

- [ ] Step 1: Write the failing test
- [ ] Step 2: Run test to verify it fails
- [ ] Step 3: Write minimal implementation
- [ ] Step 4: Run test to verify it passes
- [ ] Step 5: Commit
```

## No Placeholders

Never write: TBD, TODO, "implement later", "similar to Task N".
Every step must contain the actual content.

## Self-Review

1. Spec coverage - can you point to a task for each requirement?
2. Placeholder scan
3. Type consistency across tasks

## Execution Handoff

"Plan complete. Two options:
1. Subagent-Driven (recommended) - dispatch fresh subagent per task
2. Inline Execution - batch execution with checkpoints

Which approach?"
