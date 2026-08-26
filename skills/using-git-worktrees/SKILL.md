---
name: using-git-worktrees
description: Use when starting feature work that needs isolation from current workspace or before executing implementation plans - ensures an isolated workspace exists via native tools or git worktree fallback
---

# Using Git Worktrees

## Overview

Ensure work happens in an isolated workspace. Prefer platform's native worktree tools. Fall back to manual git worktrees only when no native tool is available.

**Adaptation note for this project:** This project does not auto-create worktrees. Per AGENTS.md: "Never create branches/worktrees without explicit user authorization." The skill's "ask for consent before creating" guidance aligns with this. Always get explicit user permission before creating a worktree.

## Step 0: Detect Existing Isolation

```bash
GIT_DIR=$(cd "$(git rev-parse --git-dir)" 2>/dev/null && pwd -P)
GIT_COMMON=$(cd "$(git rev-parse --git-common-dir)" 2>/dev/null && pwd -P)
```

- `GIT_DIR != GIT_COMMON` -> already in a linked worktree (skip creation)
- Also check: `git rev-parse --show-superproject-working-tree` to rule out submodule

Report: "Already in isolated workspace at `<path>` on branch `<name>`."

## Step 1: Create Isolated Workspace

**CRITICAL: Get explicit user consent before proceeding.**

### Native Worktree Tools (preferred)

If platform provides one, use it and skip to Step 2.

### Git Worktree Fallback

**Only with explicit user permission.**

```bash
# Check for existing worktree directory
ls -d .worktrees 2>/dev/null || ls -d worktrees 2>/dev/null

# Verify it's gitignored
git check-ignore -q .worktrees

# Create worktree
git worktree add .worktrees/<branch-name> -b <branch-name>
```

## Step 2: Project Setup

Auto-detect and run appropriate setup:
```bash
# CMake project
cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j$(nproc)
```

## Step 3: Verify Clean Baseline

Run tests to ensure clean start:
```bash
ctest --test-dir build -R "test_net_info|test_quality|test_anomaly|test_database"
```

If tests fail: Report failures, ask whether to proceed or investigate.

## Quick Reference

| Situation | Action |
|-----------|--------|
| Already in linked worktree | Skip creation |
| In submodule | Treat as normal repo |
| Native worktree tool available | Use it |
| No native tool + user consent | Git worktree fallback |
| No user consent | Work in place |
| Tests fail during baseline | Report + ask |

## Common Rationalizations

| Excuse | Reality |
|--------|---------|
| "Obviously not in a worktree" | Run Step 0. Detection commands settle it. |
| "git worktree add is quicker" | Native tools own placement and cleanup. |
| "Directory is surely ignored" | Run git check-ignore. |
| "Workspace is fresh - skip baseline" | Dirty baseline makes every later failure ambiguous. |
