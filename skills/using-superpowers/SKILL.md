---
name: using-superpowers
description: Use when starting any conversation - establishes how to find and use skills, requiring skill invocation before ANY response including clarifying questions
---

<SUBAGENT-STOP>
If you were dispatched as a subagent to execute a specific task, ignore this skill.
</SUBAGENT-STOP>

<EXTREMELY-IMPORTANT>
If you think there is even a 1% chance a skill might apply to what you are doing, you ABSOLUTELY MUST invoke the skill.

IF A SKILL APPLIES TO YOUR TASK, YOU DO NOT HAVE A CHOICE. YOU MUST USE IT.

This is not negotiable. You cannot rationalize your way out of this.
</EXTREMELY-IMPORTANT>

## The Rule

**Invoke relevant or requested skills BEFORE any response or action** — including clarifying questions, exploring the codebase, or checking files. If it turns out wrong for the situation, you don't have to use it.

**Before entering plan mode:** if you haven't already brainstormed, invoke the brainstorming skill first.

Then announce "Using [skill] to [purpose]" and follow the skill exactly. If it has a checklist, create a todo per item.

## Skill Priority

When multiple skills apply, process skills come first — they set the approach, then implementation skills carry it out.

- "Let's build X" → brainstorming first, then implementation skills.
- "Fix this bug" → systematic-debugging first, then domain skills.

## Available Skills

**Testing:**
- **test-driven-development** — RED-GREEN-REFACTOR cycle (includes testing anti-patterns reference)

**Debugging:**
- **systematic-debugging** — 4-phase root cause process (includes root-cause-tracing, defense-in-depth, condition-based-waiting techniques)
- **verification-before-completion** — Ensure it's actually fixed

**Collaboration:**
- **requesting-code-review** — Pre-review checklist
- **receiving-code-review** — Responding to feedback

**Meta:**
- **writing-skills** — Create new skills following best practices
- **using-superpowers** — This skill; introduction to the skills system

## How to Find Skills

Skills are located in the `skills/` directory at the project root. Each skill is in its own subdirectory with a `SKILL.md` file.

**Available skills in this project:**
```
skills/
├── test-driven-development/     # Write tests first, watch them fail
├── systematic-debugging/        # 4-phase root cause investigation
├── verification-before-completion/ # Evidence before claims
├── requesting-code-review/      # Dispatch code reviewer
├── receiving-code-review/       # Handle review feedback
├── using-superpowers/          # This meta-skill
└── writing-skills/             # Create new skills
```

## Red Flags

These thoughts mean STOP—you're rationalizing:

| Thought | Reality |
|---------|---------|
| "This is just a simple question" | Questions are tasks. Check for skills. |
| "I need more context first" | Skill check comes BEFORE clarifying questions. |
| "Let me explore the codebase first" | Skills tell you HOW to explore. Check first. |
| "I can check git/files quickly" | Files lack conversation context. Check for skills. |
| "Let me gather information first" | Skills tell you HOW to gather information. |
| "This doesn't need a formal skill" | If a skill exists, use it. |
| "I remember this skill" | Skills evolve. Read current version. |
| "This doesn't count as a task" | Action = task. Check for skills. |
| "The skill is overkill" | Simple things become complex. Use it. |
| "I'll just do this one thing first" | Check BEFORE doing anything. |
| "This feels productive" | Undisciplined action wastes time. Skills prevent this. |

## User Instructions

User instructions (CLAUDE.md, AGENTS.md, direct requests) take precedence over skills, which in turn override default behavior. Only skip skill workflows or instructions when your human partner has explicitly told you to.
