---
name: finishing-a-development-branch
description: Use when implementation is complete, all tests pass, and you need to decide how to integrate the work
---

# Finishing a Development Branch

## Overview

Verify tests -> Detect environment -> Present options -> Execute choice -> Clean up.

**Adaptation note for this project:** For OpenSpec changes, use `scripts/change_archive.sh` instead of the skill's merge/PR workflow. The skill's verification and cleanup steps still apply for work done outside the OpenSpec lifecycle. Per AGENTS.md: "Never create branches/worktrees, commit, merge, push, stash, reset, clean up worktrees or install dependencies without explicit user authorization."

## Step 1: Verify Tests

Run full test suite:
```bash
ctest --test-dir build -R "test_net_info|test_quality|test_anomaly|test_audio|test_band|test_serializer|test_event|test_bt_full|test_bt_monitor$|test_iface|test_logger|test_traffic|test_database"
```

If tests fail: Report failures and stop.

## Step 2: Detect Environment

```bash
GIT_DIR=$(cd "$(git rev-parse --git-dir)" 2>/dev/null && pwd -P)
GIT_COMMON=$(cd "$(git rev-parse --git-common-dir)" 2>/dev/null && pwd -P)
```

| State | Menu |
|-------|------|
| Normal repo | Standard 3 options |
| Linked worktree, named branch | Standard 3 options + cleanup |
| Detached HEAD | Reduced 2 options |

## Step 3: Determine Base Branch

Ask: "This branch split from `<best guess>` - is that correct?"

## Step 4: Present Options

**Normal repo / named-branch worktree:**
```
Implementation complete. What would you like to do?

1. Merge back to <base-branch> locally
2. Push and create a Pull Request
3. Keep the branch as-is (I'll handle it later)

Which option?
```

**Detached HEAD:**
```
Implementation complete. You're on a detached HEAD.

1. Push as new branch and create a Pull Request
2. Keep as-is (I'll handle it later)

Which option?
```

## Step 5: Execute Choice

### Option 1: Merge Locally
```bash
git checkout <base-branch>
git pull
git merge <feature-branch>
cmake -B build && cmake --build build -j$(nproc) && ctest --test-dir build
```
If tests fail on merge: stop, leave branch/worktree in place, investigate.

### Option 2: Push and Create PR
```bash
git push -u origin <feature-branch>
```
Keep worktree for PR feedback iteration.

### Option 3: Keep As-Is
Report: "Keeping branch `<name>`. Worktree preserved at `<path>`."

### Discard (only if explicitly requested)
Confirm with exact word `discard`, then force-delete.

## Step 6: Cleanup Workspace

- Normal repo: Nothing to clean up
- Superpowers-created worktree: `git worktree remove` + `git worktree prune`
- Refusal (modified/untracked files): Show user what's at stake, ask how to handle

## Quick Reference

| Option | Merge | Push | Keep | Cleanup |
|--------|-------|------|------|---------|
| 1. Merge locally | yes | - | - | yes |
| 2. Create PR | - | yes | yes | - |
| 3. Keep as-is | - | - | yes | - |
| Discard | - | - | - | yes (force) |

## For OpenSpec Changes

After completing implementation:
1. Run `scripts/change_archive.sh <name>` to archive the change
2. This is the canonical way to complete an OpenSpec change lifecycle
