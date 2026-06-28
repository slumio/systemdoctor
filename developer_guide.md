# SysPilot Developer Guide

> Everything you need to build, understand, extend, and debug SysPilot.

---

## Table of Contents

1. [Environment Setup](#1-environment-setup)
2. [Build System](#2-build-system)
3. [Project Layout](#3-project-layout)
4. [Understanding the Codebase](#4-understanding-the-codebase)
5. [Adding a New Telemetry Metric](#5-adding-a-new-telemetry-metric)
6. [Extending the Causal Graph](#6-extending-the-causal-graph)
7. [Adding a New AI Provider](#7-adding-a-new-ai-provider)
8. [Adding a New CLI Command](#8-adding-a-new-cli-command)
9. [Working with the TUI](#9-working-with-the-tui)
10. [HPC Library Usage Patterns](#10-hpc-library-usage-patterns)
11. [Memory Management Rules](#11-memory-management-rules)
12. [Testing & Debugging](#12-testing--debugging)
13. [Performance Profiling](#13-performance-profiling)
14. [CI Pipeline](#14-ci-pipeline)
15. [Common Pitfalls](#15-common-pitfalls)

---

## 1. Environment Setup

### Required packages

```bash
sudo apt-get install -y \
  build-essential g++ \
  libmimalloc-dev \
  libsimdjson-dev \
  libspdlog-dev \
  libfmt-dev \
  libtbb-dev \
  libcurl4-openssl-dev \
  linux-headers-$(uname -r)  # for Netlink struct definitions
```

### Optional (for eBPF tracing)

```bash
sudo apt-get install -y bpftrace
```

### Optional (for deep profiling)

```bash
sudo apt-get install -y linux-perf
```

### Verify toolchain

```bash
g++ --version          # must be >= 9 for C++17 + AVX2
uname -r               # must have cn_proc Netlink support (any modern kernel)
ls /proc/1/stat        # verify /proc is mounted
```

---

## 2. Build System

### Standard build

```bash
./build.sh
```

### What `build.sh` does

```bash
g++ -std=c++17 -Ofast -flto -march=native \
    -fomit-frame-pointer -funroll-loops \
    -fno-plt -ffast-math -DNDEBUG \
    -Isrc -Isrc/vendor \
    src/*.cpp src/ui/*.cpp \
    -o syspilot \
    -pthread -lmimalloc -lsimdjson -lspdlog -lfmt -ltbb \
    /lib/x86_64-linux-gnu/libcurl.so.4
```

Key flags:
| Flag | Effect |
|---|---|
| `-Ofast` | `-O3` + `-ffast-math` + `-fno-signed-zeros` |
| `-flto` | Link-time optimization (inlines across TUs) |
| `-march=native` | Enable AVX2/FMA for SIMD cosine similarity |
| `-fno-plt` | Eliminate PLT stubs for all shared library calls |
| `-DNDEBUG` | Strip `assert()` calls |

### Debug build (no optimizations)

```bash
g++ -std=c++17 -g3 -O0 -fsanitize=address,undefined \
    -Isrc -Isrc/vendor \
    src/*.cpp src/ui/*.cpp \
    -o syspilot_debug \
    -pthread -lmimalloc -lsimdjson -lspdlog -lfmt -ltbb \
    /lib/x86_64-linux-gnu/libcurl.so.4
```

> **Note:** Remove `-lmimalloc` from debug builds when using AddressSanitizer — they conflict. Use the system allocator with ASAN instead.

### Check which SIMD path is compiled

```bash
objdump -d syspilot | grep -c "vmovups\|vfmadd"   # > 0 means AVX2 active
```

---

## 3. Project Layout

```
syspilot/
│
├── build.sh                     # Build entry point
│
├── src/
│   ├── main.cpp                 # CLI argument parser + command dispatcher
│   │
│   │   ── Core subsystems ──
│   ├── daemon.cpp / daemon.h    # syspilotd: Netlink + socket server
│   ├── causal_engine.cpp / .h   # CausalTrace: graph + BFS
│   ├── telemetry.cpp / .h       # /proc parsers + system snapshot
│   │
│   │   ── AI & Output ──
│   ├── ai.cpp / ai.h            # HTTP client (Gemini SSE, Ollama stream)
│   ├── ui/streamer.cpp / .h     # Markdown → ANSI streaming renderer
│   ├── ui/tui.cpp / tui.h       # Live terminal process monitor
│   │
│   │   ── Indexing ──
│   ├── codebase.cpp / .h        # VectorDB + SIMD cosine similarity
│   ├── profiler.cpp / .h        # Thread stacks + perf sampling
│   │
│   │   ── Infrastructure ──
│   ├── config.cpp / .h          # ~/.syspilot/config.json
│   ├── safety.cpp / .h          # Command allowlist
│   ├── utils.cpp / .h           # String/file/shell helpers
│   ├── install.cpp / .h         # Shell hook installer
│   │
│   │   ── Vendored headers ──
│   ├── vendor/
│   │   ├── concurrentqueue.h    # Moodycamel ConcurrentQueue
│   │   └── tsl/
│   │       ├── robin_map.h
│   │       ├── robin_set.h
│   │       ├── robin_hash.h
│   │       └── robin_growth_policy.h
│   └── nlohmann/
│       └── json.hpp             # nlohmann/json (for response building)
│
├── .github/workflows/ci.yml     # GitHub Actions CI
├── README.md
├── ARCHITECTURE.md
├── developer_guide.md
└── CONTRIBUTING.md
```

---

## 4. Understanding the Codebase

### Start here: `main.cpp`

```cpp
int main(int argc, char* argv[]) {
    if (argc < 2) { print_help(); return 1; }

    std::string cmd = argv[1];

    if (cmd == "explain")  { return handle_explain(argc, argv); }
    if (cmd == "ask")      { return handle_ask(argc, argv); }
    if (cmd == "daemon")   { return daemon_service::run_daemon(); }
    if (cmd == "monitor")  { ui::run_monitor(); return 0; }
    if (cmd == "install")  { return handle_install(); }
    if (cmd == "config")   { return handle_config(argc, argv); }
    if (cmd == "status")   { return handle_status(); }
    if (cmd == "provider") { return handle_provider(argc, argv); }
    if (cmd == "uninstall"){ return handle_uninstall(); }
}
```

The `explain --causal` path is the most complex — it calls into `CausalGraph::build_graph()`, then `trace_root_cause()`, then `ai::query_ai_stream()`.

### Reading the causal engine

`CausalGraph` lives in `src/causal_engine.h/.cpp`. The key entry points are:

```cpp
// Build the dependency graph for a specific PID (or all PIDs if target_pid=0)
graph.build_graph(int interval_seconds, bool use_ebpf, pid_t target_pid);

// Trace backwards from a symptom node
std::vector<std::string> path = graph.trace_root_cause("pid:4582");

// Serialize for AI consumption
std::string json_chain = graph.serialize_chain_to_json(path);
```

### Understanding `string_view` safety

`GraphNode` and `GraphEdge` use `std::string_view` pointing into `CausalGraph::arena`. This is intentional and safe as long as:

1. **Never** store a `string_view` from a node/edge after calling `arena.reset()`
2. **Never** capture a `string_view` from the graph in a lambda that outlives the graph
3. **Always** use `std::string(sv)` if you need a persistent copy

---

## 5. Adding a New Telemetry Metric

**Example: Add network receive bytes per process** from `/proc/[pid]/net/dev`.

### Step 1 — Declare in `telemetry.h`

```cpp
struct ProcessTelemetry {
    // ... existing fields ...
    uint64_t net_recv_bytes = 0;   // ← add this
    uint64_t net_send_bytes = 0;   // ← add this
};
```

### Step 2 — Parse in `telemetry.cpp`

```cpp
ProcessTelemetry collect_process_telemetry(pid_t pid) {
    ProcessTelemetry pt;
    // ... existing parsing ...

    // Parse /proc/[pid]/net/dev
    std::string net_path = "/proc/" + std::to_string(pid) + "/net/dev";
    std::ifstream net_file(net_path);
    if (net_file.is_open()) {
        std::string line;
        std::getline(net_file, line); // skip header 1
        std::getline(net_file, line); // skip header 2
        while (std::getline(net_file, line)) {
            // format: "  eth0: recv_bytes 0 0 0 0 0 0 0 0 send_bytes ..."
            uint64_t rb = 0, sb = 0;
            char iface[32];
            sscanf(line.c_str(), " %31[^:]: %lu %*u %*u %*u %*u %*u %*u %*u %lu",
                   iface, &rb, &sb);
            pt.net_recv_bytes += rb;
            pt.net_send_bytes += sb;
        }
    }
    return pt;
}
```

### Step 3 — Populate `GraphNode` in `causal_engine.cpp`

```cpp
// In take_proc_snapshot(), when creating GraphNode:
node.net_recv_kb = (double)pt.net_recv_bytes / 1024.0;
```

### Step 4 — Declare in `causal_engine.h`

```cpp
struct GraphNode {
    // ... existing fields ...
    double net_recv_kb = 0.0;  // ← add to GraphNode
};
```

### Step 5 — Use in anomaly detection

```cpp
// In build_graph(), anomaly scoring section:
if (node.net_recv_kb > 10000.0) {  // > 10 MB/s
    node.is_anomalous = true;
    node.anomaly_reason = arena.alloc("High network receive rate");
}
```

---

## 6. Extending the Causal Graph

### Adding a new `EdgeType`

1. **Declare** in `causal_engine.h`:
   ```cpp
   enum class EdgeType {
       SPAWNED_BY, READS_FROM, WRITES_TO, BLOCKED_ON, CONTENDS_WITH,
       SENDS_TO   // ← new network edge type
   };
   ```

2. **Handle** in all export functions (search for all `switch (edge.type)` blocks in `causal_engine.cpp`):
   ```cpp
   case EdgeType::SENDS_TO:
       type_str = "SENDS_TO";
       color = "#818cf8";  // indigo
       break;
   ```

3. **Emit** the edge in `build_graph()`:
   ```cpp
   if (node.net_send_kb > 1000.0) {
       std::string peer_id = "socket:" + peer_ip + ":" + port;
       GraphNode peer_node;
       peer_node.id = arena.alloc(peer_id);
       peer_node.type = NodeType::RESOURCE;
       peer_node.name = arena.alloc(peer_ip);
       add_node(peer_node);
       add_edge(node.id, peer_node.id, EdgeType::SENDS_TO, "network socket");
   }
   ```

4. **Add traversal logic** in `trace_root_cause()` if the new edge type represents a causal relationship that BFS should follow.

---

## 7. Adding a New AI Provider

All provider logic lives in `src/ai.cpp`. Add a new branch in `query_ai_stream()`:

```cpp
bool query_ai_stream(const Config& config, const std::string& prompt,
                     MdStreamer& streamer) {
    std::string url;
    std::vector<std::string> headers;
    std::string payload;

    if (config.active_provider == "gemini") {
        // ... existing gemini logic ...
    }
    else if (config.active_provider == "ollama") {
        // ... existing ollama logic ...
    }
    else if (config.active_provider == "anthropic") {  // ← add here
        url = "https://api.anthropic.com/v1/messages";
        headers = {
            "x-api-key: " + config.anthropic_api_key,
            "anthropic-version: 2023-06-01",
            "content-type: application/json"
        };
        json jreq;
        jreq["model"] = "claude-3-5-sonnet-20241022";
        jreq["max_tokens"] = 8192;
        jreq["stream"] = true;
        jreq["system"] = REASONING_SYSTEM_PROMPT;
        jreq["messages"] = json::array({
            {{"role", "user"}, {"content", prompt}}
        });
        payload = jreq.dump();
    }

    // The existing curl + streamer logic handles the rest
    return stream_response(url, headers, payload, streamer);
}
```

**Also update** `src/config.h` to add the API key field:
```cpp
struct Config {
    std::string anthropic_api_key = "";
    // ...
};
```

**And update** `src/config.cpp` to serialize/deserialize it.

---

## 8. Adding a New CLI Command

### Step 1 — Add handler declaration in `main.cpp`

```cpp
// Forward declaration
static int handle_benchmark(int argc, char* argv[]);
```

### Step 2 — Register in the command router

```cpp
if (cmd == "benchmark") { return handle_benchmark(argc, argv); }
```

### Step 3 — Update the help text

```cpp
std::cout
    << "  benchmark [--pid <PID>]  Run internal performance benchmarks\n";
```

### Step 4 — Implement the handler

```cpp
static int handle_benchmark(int argc, char* argv[]) {
    Config config = config::load();
    pid_t target_pid = 0;

    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "--pid" && i + 1 < argc) {
            target_pid = std::stoi(argv[++i]);
        }
    }

    // Your benchmark logic here
    auto t0 = std::chrono::high_resolution_clock::now();
    CausalGraph graph;
    graph.build_graph(1, false, target_pid);
    auto t1 = std::chrono::high_resolution_clock::now();

    auto ms = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    std::cout << "graph build: " << ms << " µs\n";
    return 0;
}
```

---

## 9. Working with the TUI

The TUI (`src/ui/tui.cpp`) runs a **raw mode terminal event loop**. Key APIs:

### Enable/disable raw mode

```cpp
// Called at TUI start — disables line buffering and echo
static void enable_raw_mode();

// Called before exit or before spawning a subprocess (AI output)
static void restore_raw_mode();
```

> **Always** call `restore_raw_mode()` before any `std::cout` outside the TUI, or the terminal will be broken.

### Drawing primitives

```cpp
// Move cursor and print with optional ANSI style
static void print_at(int row, int col, const std::string& str, const std::string& style = "");

// Example — print red anomaly marker
print_at(row, 2, "● ANOMALOUS", "\x1b[1;31m");
```

### Adding a new display column

In `get_processes()`, add the metric to `TuiProcess`:

```cpp
struct TuiProcess {
    pid_t pid, ppid;
    std::string name, state;
    double cpu_usage_pct;
    double read_rate_kb, write_rate_kb;
    double net_recv_kb;    // ← new column
    bool is_anomalous;
};
```

In the render loop, add to the table header and row formatting:
```cpp
// Header
print_at(table_row, 3,
    "  PID  STATE  CPU%  DISK READ  DISK WRITE  NET RECV   NAME",
    "\x1b[1;90m");

// Row
fmt::format("  {:<6} {:<5} {:>5.1f}%  {:>8.1f}  {:>8.1f}  {:>8.1f}  {}",
    p.pid, p.state, p.cpu_usage_pct,
    p.read_rate_kb, p.write_rate_kb, p.net_recv_kb, p.name);
```

---

## 10. HPC Library Usage Patterns

### mimalloc — No code needed

The drop-in in `main.cpp` handles everything:
```cpp
#include <mimalloc-new-delete.h>  // Must be first include
```
All `new`, `delete`, `malloc`, `free` across the entire binary now route to mimalloc.

### simdjson — Always use padded_string

```cpp
#include <simdjson.h>

// Pattern: stack-allocated parser (reuse across calls if possible)
simdjson::ondemand::parser parser;

// REQUIRED: pad the input string for safe SIMD loads
simdjson::padded_string padded(raw_input.data(), raw_input.size());

// Parse — zero-copy, returns string_view into padded buffer
auto doc = parser.iterate(padded);

std::string_view val;
auto err = doc["key"].get_string().get(val);
if (err == simdjson::SUCCESS) {
    // use val — valid only while padded is in scope
}
```

> ⚠️ `std::string_view` values from simdjson are **only valid while the `padded_string` is alive**. Never store them beyond the scope.

### {fmt} — Use memory_buffer for zero-alloc building

```cpp
#include <fmt/format.h>

fmt::memory_buffer buf;
buf.reserve(4096);  // optional pre-reservation

fmt::format_to(std::back_inserter(buf), "key={}, val={}", key, val);
fmt::format_to(std::back_inserter(buf), ", extra={}", extra);

std::string result = fmt::to_string(buf);  // single allocation at the end
```

### TBB concurrent_hash_map — Accessor pattern

```cpp
#include <tbb/concurrent_hash_map.h>
using ProcMap = tbb::concurrent_hash_map<pid_t, ProcessNode>;

// Write (exclusive lock on bucket):
ProcMap::accessor acc;          // RAII — releases on destruction
map.insert(acc, pid);
acc->second = new_node;         // safe to modify

// Read (shared lock on bucket):
ProcMap::const_accessor cacc;
if (map.find(cacc, pid)) {
    auto name = cacc->second.name;  // safe to read
}

// Delete:
map.erase(pid);  // thread-safe, no accessor needed
```

### tsl::robin_map — Drop-in for std::unordered_map

```cpp
#include "vendor/tsl/robin_map.h"
#include "vendor/tsl/robin_set.h"

// Exactly the same API as std::unordered_map / std::unordered_set
tsl::robin_map<std::string_view, GraphNode> nodes;
nodes.reserve(1024);   // avoids rehashing on insertion

tsl::robin_set<std::string_view> visited;
visited.reserve(64);

// find() returns iterator (same as std::unordered_map)
auto it = nodes.find(key);
if (it != nodes.end()) { ... }
```

> ⚠️ `tsl::robin_map` with `string_view` keys — the keys **must remain valid for the lifetime of the map**. Never use string_view keys pointing to temporaries.

---

## 11. Memory Management Rules

SysPilot has a strict layered memory model:

### Layer 1 — Global allocator: mimalloc

All `new`/`delete`/`malloc`/`free` go through mimalloc. No special code needed.

### Layer 2 — StringArena: bulk graph strings

```
Rule: ALL strings stored in GraphNode and GraphEdge MUST come from arena.alloc()

✅ Correct:
    node.name = arena.alloc(pt.name);   // std::string → string_view in arena

❌ Wrong:
    node.name = std::string_view(pt.name);  // pt.name may be destroyed before node!
    node.name = pt.name;                    // won't compile (string_view ≠ string)
```

### Layer 3 — Stack allocation: hot paths

The daemon Netlink receive loop and socket handler use **stack-allocated buffers**:
```cpp
// Good — stack allocated, no heap pressure on hot path
char recv_buf[8192];
ssize_t len = recv(nl_fd, recv_buf, sizeof(recv_buf), 0);

// Avoid on the hot path:
std::string str(len, '\0');   // heap allocation per iteration
```

### Layer 4 — Fixed-size arrays in structs: zero-alloc events

`ProcessNode` and `ProcessEventRecord` use `char[64]` instead of `std::string`:
```cpp
struct ProcessNode {
    char name[64];   // no heap allocation when inserting into TBB map
};
```

---

## 12. Testing & Debugging

### Smoke test the daemon

```bash
# Terminal 1 — start daemon
./syspilot daemon

# Terminal 2 — query it
echo '{"request":"process_tree"}' | nc -U /tmp/syspilot.sock | python3 -m json.tool | head -30
echo '{"request":"events"}' | nc -U /tmp/syspilot.sock | python3 -m json.tool | head -20
```

### Simulate resource contention for CausalTrace

```bash
# Start a high-I/O writer process
dd if=/dev/urandom of=/tmp/test_io bs=1M count=500 &
WRITER_PID=$!

# Diagnose it
./syspilot explain --pid $WRITER_PID --causal

kill $WRITER_PID
```

### Test the TUI locally

```bash
./syspilot daemon &
./syspilot monitor
# Press Tab to cycle sort, e to explain, q to quit
```

### Run with AddressSanitizer (no mimalloc)

```bash
g++ -std=c++17 -g3 -O1 -fsanitize=address,undefined \
    -Isrc -Isrc/vendor \
    src/main.cpp src/utils.cpp src/safety.cpp src/config.cpp \
    src/telemetry.cpp src/profiler.cpp src/codebase.cpp \
    src/causal_engine.cpp src/ai.cpp \
    src/ui/streamer.cpp src/ui/tui.cpp \
    src/daemon.cpp src/install.cpp \
    -o syspilot_asan \
    -pthread -lsimdjson -lspdlog -lfmt -ltbb \
    /lib/x86_64-linux-gnu/libcurl.so.4

ASAN_OPTIONS=detect_leaks=1 ./syspilot_asan explain --pid 1
```

### Valgrind memcheck

```bash
valgrind --tool=memcheck --leak-check=full \
  ./syspilot explain --pid $$ 2>&1 | head -50
```

---

## 13. Performance Profiling

### Tracy integration (future)

Tracy profiling markers can be added with minimal overhead:
```cpp
// When Tracy support is added:
#include <tracy/Tracy.hpp>
ZoneScoped;         // function-level zone
ZoneScopedN("build_graph");
```

### perf sampling

```bash
# Record while running a causal trace
perf record -g ./syspilot explain --pid $(pgrep stress) --causal

# Generate flame graph
perf script | stackcollapse-perf.pl | flamegraph.pl > syspilot.svg
```

### Measuring JSON parse performance

```bash
# Benchmark simdjson vs nlohmann
cat > /tmp/bench_json.cpp << 'EOF'
#include <simdjson.h>
#include <chrono>
#include <iostream>
// ... benchmark code
EOF
```

### Measuring allocator performance

```bash
# Compare with/without mimalloc by swapping the linker flag
LD_PRELOAD=/lib/x86_64-linux-gnu/libmimalloc.so.2 ./syspilot explain --pid 1
LD_PRELOAD=          ./syspilot explain --pid 1
```

### Checking SIMD is active for cosine similarity

```bash
# Confirm AVX2 FMA instructions are in the binary
objdump -d syspilot | grep -A2 "cosine" | grep -c "vfmadd"
# Should return > 0 on AVX2-capable CPU with -march=native
```

---

## 14. CI Pipeline

`.github/workflows/ci.yml` runs on every push and PR to `main`/`master`:

```yaml
steps:
  - Install: build-essential g++ + HPC libraries
  - Compile: ./build.sh
  - Verify: binary exists and --help returns 0
```

### Updating the CI for new dependencies

When adding a new library, update `.github/workflows/ci.yml`:

```yaml
- name: Install Build Essentials
  run: |
    sudo apt-get update
    sudo apt-get install -y \
      build-essential g++ \
      libmimalloc-dev libsimdjson-dev \
      libspdlog-dev libfmt-dev libtbb-dev \
      libcurl4-openssl-dev \
      libnewlib-dev   # ← add new dependency here
```

---

## 15. Common Pitfalls

### 1. `string_view` dangling after `arena.reset()`

```cpp
// ❌ WRONG — sv is dangling after graph is rebuilt
auto sv = graph.nodes.begin()->second.name;
graph.build_graph(2, false, 0);  // arena.reset() called here
std::cout << sv;  // UB!

// ✅ Correct — copy to std::string before reset
std::string name = std::string(graph.nodes.begin()->second.name);
graph.build_graph(2, false, 0);
std::cout << name;
```

### 2. simdjson `string_view` outliving `padded_string`

```cpp
// ❌ WRONG
std::string_view val;
{
    simdjson::padded_string p(data, n);
    auto doc = parser.iterate(p);
    doc["key"].get_string().get(val);
}
// p is destroyed here — val is dangling!
use(val);

// ✅ Correct — copy to std::string inside scope
std::string key_value;
{
    simdjson::padded_string p(data, n);
    auto doc = parser.iterate(p);
    std::string_view sv;
    if (doc["key"].get_string().get(sv) == simdjson::SUCCESS)
        key_value = std::string(sv);
}
```

### 3. TBB accessor not released

```cpp
// ❌ WRONG — accessor holds bucket lock indefinitely
ProcMap::accessor acc;
g_process_tree.insert(acc, pid);
// ... do slow work while holding lock ...
return;  // lock released here, but blocking other writers!

// ✅ Correct — release explicitly or use scope
{
    ProcMap::accessor acc;
    g_process_tree.insert(acc, pid);
    acc->second = node;
}  // ← accessor released here
```

### 4. Calling `std::cout` inside raw TUI mode

```cpp
// ❌ WRONG — corrupts terminal
if (error_condition) {
    std::cout << "Error: something went wrong\n";  // no cursor positioning!
}

// ✅ Correct — use print_at or restore raw mode first
restore_raw_mode();
std::cerr << "Error: something went wrong\n";
enable_raw_mode();
```

### 5. `add_edge()` with non-arena string_view

```cpp
// ❌ WRONG — temporary string destroyed after the call
std::string id = "pid:" + std::to_string(pid);
add_edge(std::string_view(id), to, EdgeType::SPAWNED_BY, "");
// id is now destroyed — edge.from_id is dangling!

// ✅ Correct — always use arena.alloc()
auto from_sv = arena.alloc("pid:" + std::to_string(pid));
add_edge(from_sv, to_sv, EdgeType::SPAWNED_BY, "");
```
