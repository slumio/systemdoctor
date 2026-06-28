# Contributing to SysPilot

Thank you for contributing to SysPilot — a high-performance OS diagnostic suite. This document covers everything from filing a bug report to landing a performance-critical patch.

---

## Table of Contents

1. [Code of Conduct](#1-code-of-conduct)
2. [Ways to Contribute](#2-ways-to-contribute)
3. [Before You Start](#3-before-you-start)
4. [Development Workflow](#4-development-workflow)
5. [Coding Standards](#5-coding-standards)
6. [Performance Standards](#6-performance-standards)
7. [Commit Convention](#7-commit-convention)
8. [Pull Request Process](#8-pull-request-process)
9. [Issue Guidelines](#9-issue-guidelines)
10. [Architecture Decisions](#10-architecture-decisions)
11. [Reviewer Checklist](#11-reviewer-checklist)

---

## 1. Code of Conduct

- Be technical and precise — this is a systems-level project
- Benchmark claims before making them
- Treat maintainer and contributor time as a limited resource
- Prefer simple, correct solutions over clever ones unless benchmarks prove otherwise

---

## 2. Ways to Contribute

| Type | Examples |
|---|---|
| **Bug fix** | Fix a crash, memory leak, or incorrect `/proc` parsing |
| **Telemetry metric** | Add network I/O rates, GPU utilization, NUMA node stats |
| **Causal graph edge** | New edge type, better contention detection heuristic |
| **AI provider** | Anthropic Claude, Mistral, Groq, OpenRouter integration |
| **HPC optimization** | SIMD path, better allocator, lock-free structure |
| **TUI feature** | New column, keyboard shortcut, color scheme |
| **Documentation** | Architecture explanation, tutorial, example output |
| **CI / build** | New platform support, sanitizer runs, benchmark CI |

---

## 3. Before You Start

### Check existing issues and PRs

Search [GitHub Issues](https://github.com/yourusername/syspilot/issues) before opening a new one. For significant changes, open an issue first to discuss the design.

### Read the architecture

For any non-trivial contribution, read [ARCHITECTURE.md](ARCHITECTURE.md) in full. The memory model (`StringArena` + `string_view`), the lock-free daemon architecture, and the two-snapshot telemetry model all have correctness implications that non-obvious changes can silently break.

### Set up the development environment

Follow [developer_guide.md §1](developer_guide.md#1-environment-setup) to install all dependencies.

---

## 4. Development Workflow

### Fork and branch

```bash
# Fork the repository on GitHub, then:
git clone https://github.com/YOUR_USERNAME/syspilot.git
cd syspilot
git remote add upstream https://github.com/ORIGINAL_OWNER/syspilot.git

# Create a feature branch
git checkout -b feature/add-network-telemetry
```

### Make changes and build

```bash
# Edit source files
# ...

# Build
./build.sh

# Quick smoke test
./syspilot daemon &
./syspilot explain --pid $$ --causal
./syspilot monitor   # visual check
```

### Run the debug build before submitting

```bash
# AddressSanitizer + UBSan build (see developer_guide.md §12)
g++ -std=c++17 -g3 -O1 -fsanitize=address,undefined \
    -Isrc -Isrc/vendor \
    src/*.cpp src/ui/*.cpp \
    -o syspilot_debug \
    -pthread -lsimdjson -lspdlog -lfmt -ltbb \
    /lib/x86_64-linux-gnu/libcurl.so.4

./syspilot_debug explain --pid $$ --causal
```

No ASAN errors should occur on any normal execution path.

### Push and open a PR

```bash
git add -p          # stage changes interactively (review what you're committing)
git commit -m "feat(telemetry): add per-process network recv/send rates"
git push origin feature/add-network-telemetry
```

---

## 5. Coding Standards

### Language and Standard

- **C++17** strictly. No C++20 features (broad compiler compatibility).
- C-style `printf` only for performance-critical hot paths. Use `fmt::format` elsewhere.
- No exceptions in hot paths (daemon Netlink loop, socket handler, BFS traversal).

### Naming Conventions

| Entity | Convention | Example |
|---|---|---|
| Types / classes | `PascalCase` | `CausalGraph`, `GraphNode` |
| Functions | `snake_case` | `build_graph()`, `read_ppid()` |
| Member variables | `snake_case` | `cpu_usage_pct`, `is_anomalous` |
| Constants | `UPPER_SNAKE` | `MAX_EVENTS`, `SOCK_PATH` |
| Namespaces | `snake_case` | `telemetry::`, `daemon_service::` |
| File names | `snake_case.cpp` | `causal_engine.cpp` |

### Header discipline

```cpp
// ✅ Correct order:
// 1. Project headers (own module first)
#include "causal_engine.h"
#include "telemetry.h"
#include "utils.h"
// 2. Vendored headers
#include "vendor/tsl/robin_map.h"
// 3. System headers (alphabetical)
#include <algorithm>
#include <chrono>
#include <string_view>
// 4. HPC library headers
#include <simdjson.h>
#include <spdlog/spdlog.h>
```

### Memory rules (non-negotiable)

| Rule | Reason |
|---|---|
| All `GraphNode`/`GraphEdge` string fields → `arena.alloc()` | Prevents dangling `string_view` after `arena.reset()` |
| `string_view` from simdjson → copied before `padded_string` goes out of scope | simdjson `string_view` is backed by `padded_string` |
| No heap allocation in Netlink receive loop | Avoids allocator contention on the hot path |
| `ProcessNode.name` is `char[64]`, not `std::string` | Allows atomic TBB map updates without heap ops |

See [developer_guide.md §11](developer_guide.md#11-memory-management-rules) for the full memory model.

### Error handling

```cpp
// ✅ Return bool/optional, log to spdlog
bool read_proc_stat(pid_t pid, ProcStat& out) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        spdlog::debug("[telemetry] cannot open /proc/{}/stat: {}", pid, strerror(errno));
        return false;
    }
    // ...
    return true;
}

// ❌ Don't throw in production paths
void build_graph(...) {
    throw std::runtime_error("...");  // ← avoid; breaks catch-less callers
}
```

### Formatting

Use **4-space indentation**. Wrap at 100 columns. The project does not currently enforce `clang-format` automatically, but contributions should follow the style of the surrounding code.

---

## 6. Performance Standards

SysPilot is a performance-critical system. Any contribution that touches the hot paths listed below **must include benchmark numbers**.

### Hot paths (benchmark required)

| Path | Target | Measurement |
|---|---|---|
| Daemon socket response | < 2 ms (process_tree) | `time echo '{"request":"process_tree"}' \| nc -U /tmp/syspilot.sock` |
| `CausalGraph::build_graph()` | < 4 s (2s interval) | `time ./syspilot explain --pid $$ --causal` |
| `trace_root_cause()` | < 1 ms | Add `chrono` instrumentation locally |
| `cosine_similarity()` 1536-dim | < 1 µs (AVX2) | Micro-benchmark before/after |
| TUI frame time | < 10 ms | Add frame timer to `run_monitor()` |

### Allocation rules

- **Never** add `std::string` fields to `ProcessNode` or `ProcessEventRecord` — they go on the Netlink hot path
- **Never** use `json::parse()` in the daemon socket handler — use `simdjson::ondemand`
- **Prefer** stack allocation for buffers < 8 KiB on hot paths
- **Use** `fmt::memory_buffer` over `std::ostringstream` for JSON building

### SIMD-eligible operations

If you're adding a mathematical operation over large arrays (distance metrics, normalization, matrix ops), check if a SIMD implementation applies. The cosine similarity in `codebase.cpp` is the reference pattern — `#if defined(__AVX2__)` dispatch with SSE4 and scalar fallbacks.

---

## 7. Commit Convention

We follow **Conventional Commits** format:

```
<type>(<scope>): <short summary>

[optional body — explain the why, not the what]

[optional footer: Refs #123, Closes #456]
```

### Types

| Type | When to use |
|---|---|
| `feat` | New feature (new command, metric, provider) |
| `fix` | Bug fix (crash, incorrect output, memory error) |
| `perf` | Performance improvement (must include benchmark) |
| `refactor` | Code restructure with no behavior change |
| `docs` | Documentation only (README, ARCHITECTURE, guide) |
| `build` | Build system, CI, dependency changes |
| `test` | New tests or test fixtures |
| `chore` | Tooling, formatting, housekeeping |

### Scopes

`daemon`, `causal`, `telemetry`, `ai`, `tui`, `codebase`, `profiler`, `config`, `safety`, `utils`, `build`, `ci`, `docs`

### Examples

```
feat(telemetry): add per-process network recv/send rates from /proc/net/dev

Adds net_recv_kb and net_send_bytes to ProcessTelemetry and GraphNode.
The rates are calculated using the same two-snapshot delta model as disk I/O.

Closes #42
```

```
perf(daemon): replace locked event vector with Moodycamel ConcurrentQueue

Before: std::vector + std::mutex, ~150ns lock acquisition under load
After:  lock-free MPMC queue, ~20ns enqueue on hot Netlink path

Benchmark (8 producer threads, 1 consumer, 1M events):
  Before: 1.82 M events/s
  After:  7.14 M events/s
```

```
fix(causal): fix dangling string_view in export_graph_to_dot

edge.details is a string_view into StringArena. Concatenating it with
operator+ was calling std::string operator+(const char*, string_view)
which fails to compile on GCC 13. Use std::string(edge.details) explicitly.
```

---

## 8. Pull Request Process

### PR title format

Same as commit convention:
```
feat(daemon): add /proc/net/tcp socket peer tracking
perf(causal): switch BFS visited set from unordered_set to tsl::robin_set
fix(tui): restore terminal raw mode on SIGINT
```

### PR description template

```markdown
## Summary

What does this PR do? One paragraph maximum.

## Motivation

Why is this change needed? What problem does it solve?

## Changes

- `src/daemon.cpp`: Added socket tracking via /proc/[pid]/net/tcp
- `src/causal_engine.h`: New EdgeType::CONNECTS_TO
- `src/causal_engine.cpp`: Emit CONNECTS_TO edges in build_graph()

## Performance Impact

<!-- If touching a hot path, include benchmark numbers -->
| Operation | Before | After |
|---|---|---|
| graph build (100 processes) | 245 ms | 231 ms |

## Testing

- [ ] `./build.sh` succeeds with no warnings
- [ ] `./syspilot_debug explain --pid $$ --causal` runs clean with ASAN
- [ ] Manual TUI smoke test
- [ ] No regression in daemon response latency

## Checklist

- [ ] Follows Coding Standards (naming, memory rules)
- [ ] No `std::string` in hot-path structs
- [ ] No heap allocation in Netlink receive loop
- [ ] `string_view` safety respected (no dangling refs)
- [ ] ARCHITECTURE.md updated if design changed
- [ ] developer_guide.md updated if new extension pattern added
```

### Review timeline

- PRs are reviewed within 3–5 business days
- Performance claims require benchmark output in the PR description
- Breaking changes to the daemon protocol require a version bump in the socket format

---

## 9. Issue Guidelines

### Bug reports

Include:

```markdown
**SysPilot version:** (output of `./syspilot --version` or git SHA)
**Kernel version:** (output of `uname -r`)
**Distribution:** Ubuntu 24.04 / Debian 12 / ...
**CPU:** (output of `grep "model name" /proc/cpuinfo | head -1`)

**Steps to reproduce:**
1. Start daemon: `./syspilot daemon`
2. Run: `./syspilot explain --pid 4582 --causal`
3. Observe: [crash / wrong output / hang]

**Expected behavior:**
...

**Actual behavior:**
...

**Relevant output / stack trace:**
[paste here]
```

### Feature requests

```markdown
**Use case:**
[What problem are you trying to solve? What existing workflow does this improve?]

**Proposed API / behavior:**
[How would the user invoke this? What would the output look like?]

**Performance considerations:**
[Is this on a hot path? Any latency requirements?]

**Alternatives considered:**
[What other approaches did you consider and why did you reject them?]
```

---

## 10. Architecture Decisions

Major design decisions are documented here. New contributors should understand the rationale before proposing changes to these areas.

### Why Netlink instead of `inotify` on `/proc`?

`inotify` cannot watch `/proc` because it is a virtual filesystem. The only kernel-supported zero-polling mechanism for process lifecycle events is the Netlink Process Connector (`cn_proc`). It delivers fork/exec/exit events with sub-millisecond latency and zero CPU cost when idle.

### Why a UNIX socket daemon instead of direct `/proc` reads per command?

Direct `/proc` reads at query time have O(n_processes) cost. For a system with 400 processes, a full snapshot takes 15–75 ms. The daemon amortizes this cost by maintaining an already-updated, in-memory process tree — query latency drops to < 2 ms for any snapshot size.

### Why `string_view` + `StringArena` instead of `std::string` in `GraphNode`?

During `build_graph()`, the engine creates hundreds of `GraphNode` and `GraphEdge` objects. With `std::string`, each field would be a separate heap allocation (typically 32–48 bytes overhead). With `StringArena` + `string_view`:
- All strings for one graph snapshot live in a single contiguous slab
- `arena.reset()` recycles all memory in O(1) — no per-string `free()`
- Better cache locality during BFS traversal (adjacent nodes have adjacent string data)

### Why `tsl::robin_map` instead of `absl::flat_hash_map`?

Abseil requires building the entire Abseil library as a dependency — not suitable for a header-vendored approach. `tsl::robin_map` is a high-quality, production-tested header-only library with comparable performance characteristics and a fully compatible `std::unordered_map` API.

### Why not ncurses or FTXUI for the TUI?

ncurses is a runtime dependency that varies across distributions (different ABI versions, locale handling issues). FTXUI is not in the standard package repositories. The raw ANSI approach using `termios` raw mode requires zero external dependencies and gives direct control over rendering latency — a single `write()` syscall per frame versus ncurses' `refresh()` abstraction layer.

---

## 11. Reviewer Checklist

When reviewing pull requests, verify:

**Correctness**
- [ ] No dangling `string_view` from `StringArena` across `arena.reset()`
- [ ] No dangling `string_view` from `simdjson` beyond the `padded_string` scope
- [ ] TBB accessor released before any slow operations or function returns
- [ ] `restore_raw_mode()` called before any non-TUI output in TUI context
- [ ] No `std::string` fields added to `ProcessNode` or `ProcessEventRecord`

**Performance**
- [ ] No heap allocation in the Netlink receive loop
- [ ] No `json::parse()` (nlohmann) in the daemon socket handler (use simdjson)
- [ ] No `std::ostringstream` for JSON building in daemon (use `fmt::memory_buffer`)
- [ ] Hot-path changes have benchmark evidence in the PR description

**Safety**
- [ ] Any new OS command goes through `safety.cpp` allowlist check
- [ ] No dynamic user input passed to `system()` or `popen()`
- [ ] No reading of credential files or `/proc/[pid]/mem`

**Build**
- [ ] `./build.sh` succeeds with no new warnings (`-Wall -Wextra`)
- [ ] Debug build (`-fsanitize=address,undefined`) runs clean
- [ ] If a new system library is added, `ci.yml` is updated

**Documentation**
- [ ] New commands documented in `README.md` Usage section
- [ ] New subsystems documented in `ARCHITECTURE.md`
- [ ] New extension patterns documented in `developer_guide.md`
- [ ] New pitfalls documented in `developer_guide.md §15`
