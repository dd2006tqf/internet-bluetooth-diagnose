---
name: subagent-driven-development
description: Use when executing implementation plans with independent tasks in the current session
---

# Subagent-Driven Development

Execute plan by dispatching a fresh implementer subagent per task, a task review after each, and a broad final review.

**Adaptation note for this project:** This skill's "continuous execution" philosophy means not pausing for trivial check-ins during an approved plan. However, the project's "遇阻塞必问" rule takes precedence for actual blockers (unplanned surfaces, evidence failures, architecture questions). Record Rulings for minor decisions in the ledger.

## Core Principle

Fresh subagent per task + task review (spec + quality) + broad final review = high quality, fast iteration

## Setup

Ensure isolated workspace using using-git-worktrees skill.

Create and maintain a ledger at the plan's workspace directory. Track:
- Completed tasks
- In-progress tasks
- Rulings and decisions
- Review results

## Model Selection

Use least powerful model that handles each role:
- Mechanical tasks (1-2 files, clear spec): cheap model
- Integration tasks (multi-file coordination): standard model
- Architecture/design tasks: most capable model
- Review tasks: scale to diff size/complexity

## The Task Loop

### 1. Dispatch Implementer
- Extract task brief to a file
- Dispatch with exact requirements from the brief
- Include interfaces from earlier tasks
- Specify report file path

### 2. Implementer Works
- Implements, tests, commits, self-reviews
- Returns status, commits, test summary, concerns

### 3. Dispatch Task Reviewer
- Review spec compliance first, then code quality
- Categorize: Critical / Important / Minor
- Return strengths, issues, assessment

### 4. Address Findings
- Fix round R of 5: R<=3 resume implementer, R>=4 fresh implementer
- Dispatch scoped re-review
- Adjudicate remaining findings
- Record rulings in ledger

### 5. Continue or Finish
- More tasks? -> dispatch next implementer
- All done? -> dispatch final code reviewer

## Blockers (for this project)

STOP and ask your human partner when:
- You discover unplanned product surfaces or consumers
- Evidence recording fails (must return to Planner)
- Architecture-level questions arise
- 3+ fix rounds have failed on the same task

## Final Review

After all tasks complete:
- Dispatch final code reviewer (use requesting-code-review skill)
- Address final findings
- Use finishing-a-development-branch skill
