Usage
=====

`type-correct` is a Clang-based refactoring tool designed to automatically correctly widen integer types (e.g., `int` ->
`size_t`) consistent with their usage, preventing truncation warnings and signed/unsigned mismatches.

## Basic Use

The most common workflow is running the tool directly on source files.

```bash
# Preview changes to stdout
$ type_correct_cli myfile.cpp

# Apply changes in-place
$ type_correct_cli -in-place myfile.cpp
```

### Compiler Database

Like other LibTooling applications, `type-correct` needs to know how your code is compiled.
If you have a `compile_commands.json` (generated via CMake `CMAKE_EXPORT_COMPILE_COMMANDS=ON`):

```bash
$ type_correct_cli -p build/ src/main.cpp
```

## Workflows

### 1. Audit Source Code

Before creating a changelist, you might want to see *what* would be changed without touching disk.

```bash
$ type_correct_cli --audit --project-root=$(pwd) src/*.cpp
```

Output:

```markdown
| File          | Line | Symbol     | Old Type | New Type |
|---------------|------|------------|----------|----------|
| src/math.cpp  | 45   | i          | int      | size_t   |
| src/utils.cpp | 12   | buffer_len | int      | size_t   |
```

### 2. Iterative Global Analysis (Recommended)

In complex C++ projects, changing a header file affects multiple compilation units.
`type-correct` supports a Map-Reduce style iterative solver to ensure global consistency.

1. **Prepare**: Create a directory for intermediate facts.
   ```bash
   mkdir facts
   ```
2. **Run Iteratively**:
   ```bash
   type_correct_cli \
     --phase=iterative \
     --facts-dir=facts \
     --project-root=$(pwd) \
     --in-place \
     src/*.cpp
   ```
   This will run multiple passes until the types converge (stopped changing).

### 3. CI/CD Integration

You can generate a machine-readable report of changes.

```bash
$ type_correct_cli --in-place --report-file=changes.json src/*.cpp
```

## CLI Reference

### Core Options

| Flag                   | Description                                                                                          |
|:-----------------------|:-----------------------------------------------------------------------------------------------------|
| `-in-place`, `-i`      | Overwrite input files with formatted output.                                                         |
| `-project-root <path>` | Limits rewriting to files within this path. Essential for preventing modification of system headers. |
| `-exclude <regex>`     | Skip files matching the regex (e.g., `tests/.*`).                                                    |

### Safety Options

| Flag                           | Description                                                                                                         |
|:-------------------------------|:--------------------------------------------------------------------------------------------------------------------|
| `-enable-abi-breaking-changes` | Allow rewriting fields in structs/classes. **Warning**: Changes binary layout.                                      |
| *(Implicit)*                   | Dependency Scanning: The tool automatically scans CMakeLists.txt to detect vendored code and marks it as read-only. |

### CTU Options

| Flag                    | Description                                                                        |
|:------------------------|:-----------------------------------------------------------------------------------|
| `-phase <phase>`        | Set execution mode: `standalone` (default), `iterative`, `map`, `reduce`, `apply`. |
| `-facts-dir <dir>`      | Directory for storing analysis state (`.facts` files).                             |
| `-max-iterations <int>` | Limit for `iterative` mode (default: 10).                                          |

### Reporting Options

| Flag                  | Description                                                |
|:----------------------|:-----------------------------------------------------------|
| `-audit`              | Print markdown table of changes to stdout. No file writes. |
| `-report-file <file>` | Append JSON change records to file.                        |
