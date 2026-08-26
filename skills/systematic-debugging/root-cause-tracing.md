# Root Cause Tracing

## Overview

Bugs often manifest deep in the call stack. Your instinct is to fix where the error appears, but that's treating a symptom.

**Core principle:** Trace backward through the call chain until you find the original trigger, then fix at the source.

## When to Use

**Use when:**
- Error happens deep in execution (not at entry point)
- Stack trace shows long call chain
- Unclear where invalid data originated
- Need to find which test/code triggers the problem

## The Tracing Process

### 1. Observe the Symptom

```
Error: Database corrupted in /tmp/weaknet/history.db
```

### 2. Find Immediate Cause

**What code directly causes this?**

```cpp
// In database_manager.cpp:
int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err_msg);
if (rc != SQLITE_OK) {
    throw std::runtime_error("Database corrupted: " + err_msg);
}
```

### 3. Ask: What Called This?

```cpp
DatabaseManager::queryRecords(...)
  → called by DatabaseService::handleQuery()
  → called by D-Bus message handler
  → called by the main event loop
```

### 4. Keep Tracing Up

**What value was passed?**
- SQL string was malformed
- Malformed SQL came from unsanitized user input
- That's the source!

### 5. Find Original Trigger

**Where did the unsanitized input come from?**

```cpp
// In dbus_server.cpp:
void onMethodCall(...) {
    std::string query = message.getArgument<std::string>();
    // Never sanitized!
    databaseManager.queryRecords(query);
}
```

## Adding Stack Traces

When you can't trace manually, add instrumentation:

```cpp
// Before the problematic operation
void DatabaseManager::executeSql(const std::string& sql) {
    std::string stack;
    // Capture stack trace
    Dl_info info;
    dladdr((void*)&DatabaseManager::executeSql, &info);
    
    LOG(ERROR) << "DEBUG executeSql: sql=" << sql 
               << " caller=" << info.dli_sname;
    
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err_msg);
    // ...
}
```

**Critical:** Use `LOG(ERROR)` or `std::cerr` - not regular logger levels - for debugging.

## Finding Which Test Causes Pollution

If something appears during tests but you don't know which test:

```bash
# Run tests one by one to find polluter
for test in $(ctest --test-dir build -N | grep 'Test #' | awk '{print $3}'); do
    echo "Running: $test"
    ctest --test-dir build -R "^$test$"
    if [ $? -ne 0 ]; then
        echo "FAILED: $test"
    fi
done
```

## Real Example: Empty Database Path

**Symptom:** Database created in wrong directory

**Trace chain:**
1. `sqlite3_open()` runs with empty path ← empty parameter
2. `DatabaseManager` constructor called with empty path
3. `DatabaseService` passed empty string from config
4. Config reader returned empty value for missing key

**Root cause:** Config reader didn't validate required keys

**Fix:** Made config reader throw on missing required keys

**Also added defense-in-depth:**
- Layer 1: Config reader validates required keys
- Layer 2: DatabaseService validates path not empty
- Layer 3: DatabaseManager validates directory exists and is writable
- Layer 4: Stack trace logging before database operations

## Key Principle

```
NEVER fix just where the error appears.
Trace back to find the original trigger.
```

## Stack Trace Tips

**In tests:** Use `std::cerr` or `LOG(ERROR)` not regular logger
**Before operation:** Log before the dangerous operation, not after it fails
**Include context:** Path, parameters, environment variables, timestamps
**Capture stack:** Use backtrace or dladdr to show complete call chain
