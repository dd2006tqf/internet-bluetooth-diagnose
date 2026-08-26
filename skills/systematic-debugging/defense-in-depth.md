# Defense-in-Depth Validation

## Overview

When you fix a bug caused by invalid data, adding validation at one place feels sufficient. But that single check can be bypassed by different code paths, refactoring, or mocks.

**Core principle:** Validate at EVERY layer data passes through. Make the bug structurally impossible.

## Why Multiple Layers

Single validation: "We fixed the bug"
Multiple layers: "We made the bug impossible"

Different layers catch different cases:
- Entry validation catches most bugs
- Business logic catches edge cases
- Environment guards prevent context-specific dangers
- Debug logging helps when other layers fail

## The Four Layers

### Layer 1: Entry Point Validation

**Purpose:** Reject obviously invalid input at API boundary

```cpp
class DatabaseManager {
public:
    explicit DatabaseManager(const std::string& db_path) {
        if (db_path.empty()) {
            throw std::invalid_argument("db_path cannot be empty");
        }
        if (!std::filesystem::exists(db_path.parent_path())) {
            throw std::invalid_argument("db_path parent does not exist: " + db_path);
        }
        if (!std::filesystem::is_directory(db_path.parent_path())) {
            throw std::invalid_argument("db_path parent is not a directory: " + db_path);
        }
        db_path_ = db_path;
    }
};
```

### Layer 2: Business Logic Validation

**Purpose:** Ensure data makes sense for this operation

```cpp
class DatabaseService {
public:
    void initialize(const std::string& db_path) {
        if (db_path.empty()) {
            throw std::runtime_error("db_path required for initialization");
        }
        manager_ = std::make_unique<DatabaseManager>(db_path);
    }
};
```

### Layer 3: Environment Guards

**Purpose:** Prevent dangerous operations in specific contexts

```cpp
class DatabaseManager {
    void executeSql(const std::string& sql) {
        // In tests, refuse dangerous operations
        if (getenv("TESTING")) {
            if (sql.find("DROP TABLE") != std::string::npos && 
                sql.find("history") != std::string::npos) {
                throw std::runtime_error("Refusing DROP TABLE in test mode");
            }
        }
        // ... proceed
    }
};
```

### Layer 4: Debug Instrumentation

**Purpose:** Capture context for forensics

```cpp
void DatabaseManager::executeSql(const std::string& sql) {
    LOG(ERROR) << "About to execute SQL on: " << db_path_
               << " SQL: " << sql.substr(0, 100) << "...";
    // ... proceed
}
```

## Applying the Pattern

When you find a bug:
1. **Trace the data flow** - Where does bad value originate? Where used?
2. **Map all checkpoints** - List every point data passes through
3. **Add validation at each layer** - Entry, business, environment, debug
4. **Test each layer** - Try to bypass layer 1, verify layer 2 catches it

## Example from This Project

Bug: Empty database path caused SQLite to create DB in current working directory

**Data flow:**
1. Config file → empty string for missing key
2. `DatabaseService::initialize("")` 
3. `DatabaseManager("/tmp/weaknet/")` ← should have been the path
4. `sqlite3_open()` with empty path → uses cwd

**Four layers added:**
- Layer 1: `ConfigReader` validates required keys exist
- Layer 2: `DatabaseService` validates path not empty
- Layer 3: `DatabaseManager` validates directory exists/writable
- Layer 4: Stack trace logging before `sqlite3_open()`

**Result:** Bug impossible to reproduce. All tests pass.

## Key Insight

All four layers were necessary. During testing, each layer caught bugs the others missed:
- Different code paths bypassed entry validation
- Mocks bypassed business logic checks
- Edge cases on different platforms needed environment guards
- Debug logging identified structural misuse

**Don't stop at one validation point.** Add checks at every layer.
