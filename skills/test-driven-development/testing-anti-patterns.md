# Testing Anti-Patterns

**Load this reference when:** writing or changing tests, adding mocks, or tempted to add test-only methods to production code.

## Overview

Tests must verify real behavior, not mock behavior. Mocks are a means to isolate, not the thing being tested.

**Core principle:** Test what the code does, not what the mocks do.

**Following strict TDD prevents these anti-patterns.**

## The Iron Laws

```
1. NEVER test mock behavior
2. NEVER add test-only methods to production classes
3. NEVER mock without understanding dependencies
```

## Anti-Pattern 1: Testing Mock Behavior

**The violation:**

```cpp
// ❌ BAD: Testing that the mock exists
TEST(SidebarTest, RendersSidebar) {
    auto page = std::make_unique<Page>();
    EXPECT_EQ(page->getTestId(), "sidebar-mock");
}
```

**Why this is wrong:**

- You're verifying the mock works, not that the component works
- Test passes when mock is present, fails when it's not
- Tells you nothing about real behavior

**The fix:**

```cpp
// ✅ GOOD: Test real component or don't mock it
TEST(SidebarTest, RendersSidebar) {
    auto page = std::make_unique<Page>();  // Don't mock sidebar
    EXPECT_TRUE(page->hasNavigationRole());
}
```

### Gate Function

```
BEFORE asserting on any mock element:
  Ask: "Am I testing real component behavior or just mock existence?"

  IF testing mock existence:
    STOP - Delete the assertion or unmock the component
    Test real behavior instead
```

## Anti-Pattern 2: Test-Only Methods in Production

**The violation:**

```cpp
// ❌ BAD: destroy() only used in tests
class Session {
public:
    void destroy() {  // Looks like production API!
        workspaceManager_->destroyWorkspace(id_);
        // ... cleanup
    }
};

// In tests:
session->destroy();
```

**Why this is wrong:**

- Production class polluted with test-only code
- Dangerous if accidentally called in production
- Violates YAGNI and separation of concerns
- Confuses object lifecycle with entity lifecycle

**The fix:**

```cpp
// ✅ GOOD: Test utilities handle test cleanup
// Session has no destroy() - it's stateless in production

// In test_utils/:
void cleanupSession(Session& session, WorkspaceManager& workspaceManager) {
    auto info = session.getWorkspaceInfo();
    if (info.has_value()) {
        workspaceManager.destroyWorkspace(info->id);
    }
}

// In tests:
cleanupSession(session, workspaceManager);
```

### Gate Function

```
BEFORE adding any method to production class:
  Ask: "Is this only used by tests?"

  IF yes:
    STOP - Don't add it
    Put it in test utilities instead

  Ask: "Does this class own this resource's lifecycle?"

  IF no:
    STOP - Wrong class for this method
```

## Anti-Pattern 3: Mocking Without Understanding

**The violation:**

```cpp
// ❌ BAD: Mock breaks test logic
TEST(ServerTest, DetectsDuplicateServer) {
    // Mock prevents config write that test depends on!
    EXPECT_CALL(catalogMock, discoverAndCacheTools())
        .WillRepeatedly(Return());

    addServer(config);
    addServer(config);  // Should throw - but won't!
}
```

**Why this is wrong:**

- Mocked method had side effect test depended on (writing config)
- Over-mocking to "be safe" breaks actual behavior
- Test passes for wrong reason or fails mysteriously

**The fix:**

```cpp
// ✅ GOOD: Mock at correct level
TEST(ServerTest, DetectsDuplicateServer) {
    // Mock the slow part, preserve behavior test needs
    EXPECT_CALL(serverManagerMock, slowStart())
        .WillRepeatedly(Return());

    addServer(config);  // Config written
    addServer(config);  // Duplicate detected ✓
}
```

### Gate Function

```
BEFORE mocking any method:
  STOP - Don't mock yet

 1. Ask: "What side effects does the real method have?"
 2. Ask: "Does this test depend on any of those side effects?"
 3. Ask: "Do I fully understand what this test needs?"

  IF depends on side effects:
    Mock at lower level (the actual slow/external operation)
    OR use test doubles that preserve necessary behavior
    NOT the high-level method the test depends on

  IF unsure what test depends on:
    Run test with real implementation FIRST
    Observe what actually needs to happen
    THEN add minimal mocking at the right level
```

## Anti-Pattern 4: Incomplete Mocks

**The violation:**

```cpp
// ❌ BAD: Partial mock - only fields you think you need
auto mockResponse = Response{
    .status = "success",
    .data = {.userId = "123", .name = "Alice"}
    // Missing: metadata that downstream code uses
};

// Later: breaks when code accesses response.metadata.requestId
```

**Why this is wrong:**

- Partial mocks hide structural assumptions
- Downstream code may depend on fields you didn't include
- Silent failures
- Tests pass but integration fails

**The fix:**

```cpp
// ✅ GOOD: Mirror real structure completeness
auto mockResponse = Response{
    .status = "success",
    .data = {.userId = "123", .name = "Alice"},
    .metadata = {.requestId = "req-789", .timestamp = 1234567890}
    // All fields real API returns
};
```

## Anti-Pattern 5: Integration Tests as Afterthought

**The violation:**

```
✅ Implementation complete
❌ No tests written
"Ready for testing"
```

**Why this is wrong:**

- Testing is part of implementation, not optional follow-up
- TDD would have caught this
- Can't claim complete without tests

**The fix:**

```
TDD cycle:
1. Write failing test
2. Implement to pass
3. Refactor
4. THEN claim complete
```

## When Mocks Become Too Complex

**Warning signs:**

- Mock setup longer than test logic
- Mocking everything to make test pass
- Mocks missing methods real components have
- Test breaks when mock changes

**Consider:** Integration tests with real components often simpler than complex mocks.

## TDD Prevents These Anti-Patterns

**Why TDD helps:**

1. **Write test first** → Forces you to think about what you're actually testing
2. **Watch it fail** → Confirms test tests real behavior, not mocks
3. **Minimal implementation** → No test-only methods creep in
4. **Real dependencies** → You see what the test actually needs before mocking

## Quick Reference

| Anti-Pattern | Fix |
|---|---|
| Assert on mock elements | Test real component or unmock it |
| Test-only methods in production | Move to test utilities |
| Mock without understanding | Understand dependencies first, mock minimally |
| Incomplete mocks | Mirror real API completely |
| Tests as afterthought | TDD - tests first |
| Over-complex mocks | Consider integration tests |

## Red Flags

- Assertion checks for `*-mock` test IDs
- Methods only called in test files
- Mock setup is >50% of test
- Test fails when you remove mock
- Can't explain why mock is needed
- Mocking "just to be safe"

## The Bottom Line

**Mocks are tools to isolate, not things to test.**

If TDD reveals you're testing mock behavior, you've gone wrong.

Fix: Test real behavior or question why you're mocking at all.
