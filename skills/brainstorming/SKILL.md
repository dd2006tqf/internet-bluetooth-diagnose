---
name: brainstorming
description: "You MUST use this before any creative work - creating features, building components, adding functionality, or modifying behavior. Explores user intent, requirements and design before implementation."
---

# Brainstorming Ideas Into Designs

Help turn ideas into fully formed designs and specs through natural collaborative dialogue.

**Adaptation note for this project:** When working within an OpenSpec change, this skill's techniques complement the Planner role's design phase. The brainstorming process maps directly to the Planner's proposal.md -> design.md -> specs/ workflow.

<HARD-GATE>
Do NOT invoke any implementation skill, write any code, or take any implementation action until you have told your human partner what you intend and they have approved it.
</HARD-GATE>

## Three Paths

- **Spike** - feasibility question. Output is an answer, not code you keep.
- **Bounded** - well-scoped change to existing code. Short design in chat, then approval.
- **Architectural** - new subsystems or structural changes. Full process with design doc.

When in doubt between two paths, take the heavier one.

## Checklist

**Spike:** Explore context -> Present question -> Get approval -> Investigate -> Report findings
**Bounded:** Explore context -> Ask questions -> Present short design -> Get approval -> Implement
**Architectural:** Explore context -> Ask questions -> Propose approaches -> Present design -> Write design doc -> User reviews -> Invoke writing-plans

## Red Flags

| Thought | Reality |
|---------|---------|
| "This is too simple to need a design" | Simple means short design, not no design. |
| "I'll skip the design" | Reaching for a label to skip work IS the doubt. |
| "Design is obvious - start while they read" | The gate is the approval, not the length. |

## Process

1. Check current project state (files, docs, recent commits)
2. Assess scope - flag if request covers multiple subsystems
3. Ask questions one at a time
4. Propose 2-3 different approaches with trade-offs
5. Present design, ask after each section
6. Write validated design to docs/superpowers/specs/ or OpenSpec design.md
7. Self-review: placeholder scan, consistency, scope, ambiguity
8. User reviews written spec
9. Transition to writing-plans skill

## Terminal States

- Architectural: ONLY invoke writing-plans after brainstorming
- Bounded: approval -> normal development workflow, no plan doc
- Spike: terminal state is a reported recommendation
