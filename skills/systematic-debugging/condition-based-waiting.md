# Condition-Based Waiting

## Overview

Flaky tests often guess at timing with arbitrary delays. This creates race conditions where tests pass on fast machines but fail under load or in CI.

**Core principle:** Wait for the actual condition you care about, not a guess about how long it takes.

## When to Use

**Use when:**
- Tests have arbitrary delays (`sleep_for`, `usleep`, `std::this_thread::sleep_for`)
- Tests are flaky (pass sometimes, fail under load)
- Tests timeout when run in parallel
- Waiting for async operations to complete

**Don't use when:**
- Testing actual timing behavior (debounce, throttle intervals)
- Always document WHY if using arbitrary timeout

## Core Pattern

```cpp
// ❌ BEFORE: Guessing at timing
std::this_thread::sleep_for(std::chrono::milliseconds(50));
auto result = getResult();
EXPECT_TRUE(result.has_value());

// ✅ AFTER: Waiting for condition
auto result = waitForCondition([&]() { return getResult().has_value(); },
                               "result to be available",
                               5000);  // 5 second timeout
EXPECT_TRUE(result.has_value());
```

## Quick Patterns

| Scenario | Pattern |
|----------|---------|
| Wait for event | `waitForCondition([&]() { return eventFired; }, "event")` |
| Wait for state | `waitForCondition([&]() { return machine.state() == Ready; }, "ready state")` |
| Wait for count | `waitForCondition([&]() { return items.size() >= 5; }, "5 items")` |
| Wait for file | `waitForCondition([&]() { return std::filesystem::exists(path); }, "file")` |
| Complex condition | `waitForCondition([&]() { return obj.ready() && obj.value() > 10; }, "ready with value")` |

## Implementation

Generic polling utility:

```cpp
template <typename Predicate>
void waitForCondition(Predicate condition, 
                      const std::string& description,
                      int timeoutMs = 5000,
                      int pollIntervalMs = 10) {
    auto startTime = std::chrono::steady_clock::now();
    while (true) {
        if (condition()) return;
        
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startTime).count();
        
        if (elapsed > timeoutMs) {
            throw std::runtime_error(
                "Timeout waiting for " + description + 
                " after " + std::to_string(timeoutMs) + "ms");
        }
        
        std::this_thread::sleep_for(
            std::chrono::milliseconds(pollIntervalMs));
    }
}
```

## Common Mistakes

**❌ Polling too fast:** `sleep_for(1ms)` - wastes CPU
**✅ Fix:** Poll every 10ms or more

**❌ No timeout:** Loop forever if condition never met
**✅ Fix:** Always include timeout with clear error

**❌ Stale data:** Cache state before loop
**✅ Fix:** Call getter inside loop for fresh data

## When Arbitrary Timeout IS Correct

```cpp
// Service ticks every 10 seconds - need 2 ticks to verify
waitForCondition([&]() { return service.tickCount() >= 2; },
                 "2 ticks completed");
// Then wait for specific timing behavior
std::this_thread::sleep_for(std::chrono::seconds(1));
// 1 second = 1 tick at 10 second intervals - documented and justified
```

**Requirements:**
1. First wait for triggering condition
2. Based on known timing (not guessing)
3. Comment explaining WHY

## Real-World Impact

From debugging sessions:
- Fixed flaky tests across multiple test files
- Pass rate: 60% → 100%
- Execution time: 40% faster
- No more race conditions
