# Skill Authoring Best Practices

> Learn how to write effective Skills that agents can discover and use successfully.

Good Skills are concise, well-structured, and tested with real usage. This guide provides practical authoring decisions to help you write Skills that agents can discover and use effectively.

## Core Principles

### Concise is key

The context window is a public good. Your Skill shares the context window with everything else your agent needs to know.

Not every token in your Skill has an immediate cost. At startup, only the metadata (name and description) from all Skills is pre-loaded. Agents read SKILL.md only when the Skill becomes relevant, and read additional files only as needed.

**Default assumption:** Agents are already very smart. Only add context agents don't already have.

### Set appropriate degrees of freedom

Match the level of specificity to the task's fragility and variability.

**High freedom** (text-based instructions):
Use when: Multiple approaches are valid. Decisions depend on context. Heuristics guide the approach.

**Medium freedom** (pseudocode or scripts with parameters):
Use when: A preferred pattern exists. Some variation is acceptable.

**Low freedom** (specific scripts, few or no parameters):
Use when: Operations are fragile and error-prone. Consistency is critical. A specific sequence must be followed.

### Test with all models you plan to use

Skills act as additions to models, so effectiveness depends on the underlying model. Test your Skill with all the models you plan to use it with.

## Skill Structure

### Naming conventions

Use consistent naming patterns. We recommend using gerund form (verb + -ing) for Skill names.

**Good naming examples:**
- "processing-pdfs"
- "analyzing-spreadsheets"
- "managing-databases"
- "testing-code"
- "writing-documentation"

### Writing effective descriptions

The `description` field enables Skill discovery. Write in third person. Be specific and include key terms an agent would search for.

```yaml
description: Extract text and tables from PDF files, fill forms, merge documents. Use when working with PDF files or when the user mentions PDFs, forms, or document extraction.
```

Avoid vague descriptions like:
```yaml
description: Helps with documents
```

### Progressive disclosure patterns

SKILL.md serves as an overview that points agents to detailed materials as needed.

**Pattern 1: High-level guide with references**
```markdown
## Quick start
Use [library] for [task]:
[code example]

## Advanced features
See [reference file] for complete guide
```

**Pattern 2: Domain-specific organization**
For Skills with multiple domains, organize content by domain to avoid loading irrelevant context.

**Pattern 3: Conditional details**
Show basic content, link to advanced content:
```markdown
## Basic usage
Use [tool] for [purpose].

## Advanced features
For tracked changes: See [advanced.md]
```

### Avoid deeply nested references

Keep references one level deep from SKILL.md. All reference files should link directly from SKILL.md.

## Testing Your Skill

### Testing all skill types

**Discipline-Enforcing Skills (rules/requirements)**
- Test with: Academic questions, pressure scenarios, multiple pressures combined
- Success criteria: Agent follows rule under maximum pressure

**Technique Skills (how-to guides)**
- Test with: Application scenarios, variation scenarios, missing information tests
- Success criteria: Agent successfully applies technique to new scenario

**Pattern Skills (mental models)**
- Test with: Recognition scenarios, application scenarios, counter-examples
- Success criteria: Agent correctly identifies when/how to apply pattern

**Reference Skills (documentation/APIs)**
- Test with: Retrieval scenarios, application scenarios, gap testing
- Success criteria: Agent finds and correctly applies reference information

### Common rationalizations for skipping testing

| Excuse | Reality |
|--------|---------|
| "Skill is obviously clear" | Clear to you ≠ clear to other agents. Test it. |
| "It's just a reference" | References can have gaps. Test retrieval. |
| "Testing is overkill" | Untested skills have issues. Always. |
| "I'll test if problems emerge" | Problems = agents can't use skill. Test BEFORE deploying. |
| "Too tedious to test" | Testing is less tedious than debugging bad skill in production. |
