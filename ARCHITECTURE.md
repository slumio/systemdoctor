# SysPilot Architecture

> Deep-dive into the internal design, data flow, memory model, and HPC library choices of SysPilot.

---

## Table of Contents

1. [System Overview](#1-system-overview)
2. [Component Map](#2-component-map)
3. [syspilotd Daemon](#3-syspilotd-daemon)
4. [CausalTrace Engine](#4-causaltrace-engine)
5. [StringArena Memory Model](#5-stringarena-memory-model)
6. [Telemetry Subsystem](#6-telemetry-subsystem)
7. [AI Reasoning Layer](#7-ai-reasoning-layer)
8. [Terminal UI (TUI)](#8-terminal-ui-tui)
9. [Vector Codebase Index](#9-vector-codebase-index)
10. [HPC Library Stack](#10-hpc-library-stack)
11. [Data Flow — End to End](#11-data-flow--end-to-end)
12. [Security Model](#12-security-model)
13. [Performance Targets](#13-performance-targets)

---

## 1. System Overview

SysPilot is a **single-binary** Linux diagnostic suite with three concurrent execution models:

```
┌─────────────────────────────────────────────────────────────────┐
│                       syspilot binary                           │
│                                                                 │
│   ┌─────────────────┐  ┌──────────────────┐  ┌──────────────┐  │
│   │  CLI commands   │  │  syspilotd mode  │  │  TUI monitor │  │
│   │  (synchronous)  │  │  (daemon server) │  │  (event loop)│  │
│   └────────┬────────┘  └────────┬─────────┘  └──────┬───────┘  │
│            │                   │                    │           │
│            └───────────────────┼────────────────────┘           │
│                                │                                │
│            ┌───────────────────▼────────────────────┐           │
│            │         UNIX socket /tmp/syspilot.sock  │           │
│            └───────────────────┬────────────────────┘           │
│                                │                                │
│            ┌───────────────────▼────────────────────┐           │
│            │           CausalTrace Engine            │           │
│            │   multigraph · BFS · arena allocator    │           │
│            └───────────────────┬────────────────────┘           │
│                                │                                │
│       ┌────────────────────────▼────────────────────────────┐   │
│       │                   AI Layer                          │   │
│       │     Gemini API / local Ollama → MdStreamer          │   │
│       └────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
```

The key architectural decision is the **daemon-consumer split**: `syspilotd` maintains a continuously-updated, zero-copy view of the Linux process tree with no polling. CLI commands and the TUI are light consumers that query this view over a UNIX socket in microseconds.

---

## 2. Component Map

```
src/
├── main.cpp              ← CLI router & top-level orchestration
├── daemon.cpp            ← syspilotd: Netlink listener + socket server
├── causal_engine.cpp     ← CausalTrace: multigraph construction + BFS
├── telemetry.cpp         ← /proc parsers (stat, fd, diskstats, meminfo)
├── ai.cpp                ← HTTP AI client (Gemini SSE / Ollama stream)
├── codebase.cpp          ← VectorDB + SIMD cosine similarity
├── profiler.cpp          ← Thread stacks + perf sampling
├── config.cpp            ← JSON config (~/.syspilot/config.json)
├── safety.cpp            ← Command allowlist enforcement
├── utils.cpp             ← String/file/shell utilities
├── install.cpp           ← Shell hook installer
└── ui/
    ├── tui.cpp           ← Raw ANSI terminal event loop
    └── streamer.cpp      ← Markdown → ANSI streaming renderer
```

**Dependency graph** (arrows = "depends on"):

```
main ──► causal_engine ──► telemetry
    ──► ai             ──► utils
    ──► daemon
    ──► codebase       ──► utils
    ──► profiler
    ──► ui/tui         ──► causal_engine
                       ──► ai
    ──► config
    ──► safety
```

---

## 3. syspilotd Daemon

### 3.1 Netlink Process Connector

The daemon subscribes to the Linux kernel's **Netlink Connector** (`NETLINK_CONNECTOR`, group `CN_IDX_PROC`) which delivers process lifecycle events with zero polling:

```
Linux Kernel
│
│  (PROC_EVENT_FORK)   child_pid, parent_pid
│  (PROC_EVENT_EXEC)   process_pid
│  (PROC_EVENT_EXIT)   process_pid
│
└──► PF_NETLINK socket recv() ──► daemon recv buffer (8 KiB, stack-allocated)
```

No `inotify`, no `/proc` polling, no timers. The kernel pushes events the moment they happen, giving **sub-millisecond latency** from process creation to in-memory update.

```cpp
// Subscription message (sent once at startup)
*op = PROC_CN_MCAST_LISTEN;
send(nl_fd, nlh, nlh->nlmsg_len, 0);

// Receive loop — blocking recv, zero allocation on hot path
ssize_t len = recv(nl_fd, recv_buf, sizeof(recv_buf), 0);
auto* ev = (struct proc_event*)cn_r->data;
switch (ev->what) {
case PROC_EVENT_FORK: ...
case PROC_EVENT_EXEC: ...
case PROC_EVENT_EXIT: ...
}
```

### 3.2 Process Tree — `tbb::concurrent_hash_map`

```cpp
using ProcMap = tbb::concurrent_hash_map<pid_t, ProcessNode>;
static ProcMap g_process_tree;
```

- **Bucket-level locking**: concurrent writers on different PIDs never block each other
- **Concurrent readers**: multiple socket threads can read simultaneously without any lock acquisition
- `ProcessNode` uses fixed-size `char[64]` for the name field — **no heap allocation on the Netlink hot path**

### 3.3 Event Buffer — `moodycamel::ConcurrentQueue`

```cpp
static moodycamel::ConcurrentQueue<ProcessEventRecord> g_event_queue;

// Netlink thread (producer):
g_event_queue.enqueue(rec);     // never blocks, O(1) amortized

// Socket handler (consumer):
ProcessEventRecord rec;
while (g_event_queue.try_dequeue(rec)) { ... }  // wait-free drain
```

The `ProcessEventRecord` struct uses fixed-size `char` arrays to stay allocation-free in the enqueue path.

### 3.4 Socket Server — simdjson + {fmt}

**Request parsing** uses `simdjson::ondemand`:
```cpp
simdjson::padded_string padded(raw, n);
auto doc = parser.iterate(padded);       // zero-copy, SIMD parse
std::string_view req_type;
doc["request"].get_string().get(req_type);  // string_view into raw buffer
```

**Response building** uses `fmt::memory_buffer`:
```cpp
fmt::memory_buffer buf;
fmt::format_to(std::back_inserter(buf),
    R"({{"pid":{},"ppid":{},"name":"{}","state":"{}"}})",
    n.pid, n.ppid, n.name, n.state);
```

The `memory_buffer` uses **SSO** (small-buffer optimization) — no heap allocation for responses under ~512 bytes.

---

## 4. CausalTrace Engine

### 4.1 Graph Primitives

```cpp
// All string fields are string_view into StringArena
// Zero heap allocation per node/edge

struct GraphNode {
    std::string_view id;          // "pid:1234" or "resource:/path/to/file"
    NodeType         type;        // PROCESS | RESOURCE
    std::string_view name;
    pid_t            pid;
    std::string_view state;       // "R", "S", "D", "Z"
    double           cpu_usage_pct;
    double           read_rate_kb;
    double           write_rate_kb;
    bool             is_anomalous;
    std::string_view anomaly_reason;
};

struct GraphEdge {
    std::string_view from_id;
    std::string_view to_id;
    EdgeType         type;        // SPAWNED_BY | READS_FROM | WRITES_TO
                                  // BLOCKED_ON | CONTENDS_WITH
    std::string_view details;
};
```

The node store is a `tsl::robin_map<string_view, GraphNode>` — open-addressing with robin-hood probing gives **3–5× better cache locality** than `std::unordered_map` on dense lookup workloads.

### 4.2 Graph Construction — `build_graph()`

`build_graph()` is a **two-pass delta analysis**:

```
Pass 1: take_proc_snapshot() → snap1 + disk1
  Sleep(interval_seconds)
Pass 2: take_proc_snapshot() → snap2 + disk2

Delta computation:
  cpu_usage_pct = (Δutime + Δstime) / clk_tck / interval
  read_rate_kb  = (snap2.read_bytes  - snap1.read_bytes)  / interval / 1024
  write_rate_kb = (snap2.write_bytes - snap1.write_bytes) / interval / 1024
```

Node addition in priority order:
1. **Process nodes** — all PIDs surviving both snapshots
2. **Parent relationships** — `SPAWNED_BY` edges from PPID
3. **Open file descriptors** — filtered from `/proc/[pid]/fd/` symlinks
4. **Block device nodes** — from `/proc/diskstats` delta
5. **Socket/pipe nodes** — from fd symlink type detection
6. **eBPF trace log** — parsed if `bpftrace` was running
7. **Contention detection** — `CONTENDS_WITH` edges between processes sharing high-I/O resources

### 4.3 Root Cause Traversal — `trace_root_cause()`

Reverse-BFS from the symptom node:

```
Input: start_node_id (e.g., "pid:4582")

Queue: [pid:4582]
Visited: {pid:4582}  ← tsl::robin_set (open-addressing)

Loop:
  current = dequeue()
  path.append(current)
  For each edge where edge.from_id == current:
    if edge.type ∈ {BLOCKED_ON, SPAWNED_BY, CONTENDS_WITH}:
      if neighbor not visited: enqueue(neighbor)
  For each edge where current is a RESOURCE:
    Walk WRITES_TO/READS_FROM back to process nodes
    Rank by total I/O rate (active_writers sort)
    Add highest-impact writers to queue

Output: ordered path [symptom → ... → root cause]
```

The `tsl::robin_set<string_view>` for `visited` avoids hash collisions that plague `std::unordered_set` under dense string keys.

### 4.4 Graph Export Formats

| Method | Format | Use Case |
|---|---|---|
| `serialize_chain_to_json()` | JSON | AI context payload |
| `export_graph_to_dot()` | Graphviz DOT | `dot -Tsvg` rendering |
| `export_graph_to_mermaid()` | Mermaid | GitHub/Notion embedding |
| `export_graph_to_html()` | Cytoscape.js HTML | Interactive browser visualization |

---

## 5. StringArena Memory Model

All string data in the causal graph lives in a **bump allocator**:

```cpp
class StringArena {
    std::vector<std::vector<char>> chunks;  // slab list
    size_t chunk_size     = 256 * 1024;    // 256 KiB default slab
    size_t current_chunk  = 0;
    size_t current_offset = 0;

public:
    std::string_view alloc(std::string_view str) {
        // Bump pointer allocation — O(1), cache-line-friendly sequential writes
        char* dest = &chunks[current_chunk][current_offset];
        memcpy(dest, str.data(), str.size());
        current_offset += str.size();
        return {dest, str.size()};
    }

    void reset() { current_chunk = 0; current_offset = 0; }  // O(1) "free all"
};
```

**Key properties:**
- All `GraphNode` and `GraphEdge` string fields are `std::string_view` pointing into the arena
- `arena.reset()` at the start of each `build_graph()` call recycles all memory in O(1)
- Sequential allocation = cache-friendly; strings for the same graph snapshot live near each other in memory
- No per-string `free()` — the allocator lifecycle matches the graph lifecycle

---

## 6. Telemetry Subsystem

`src/telemetry.cpp` is a direct `/proc` filesystem parser with no external dependencies.

### Parsed Sources

| Source | Metrics |
|---|---|
| `/proc/[pid]/stat` | utime, stime, state, ppid, thread count, minor/major faults |
| `/proc/[pid]/io` | read_bytes, write_bytes, read_char, write_char, syscr, syscw |
| `/proc/[pid]/status` | VmRSS, VmSize |
| `/proc/[pid]/schedstat` | run_ticks, wait_ticks, timeslices |
| `/proc/[pid]/environ` | environment variable dump |
| `/proc/[pid]/fd/` | open file descriptors (symlink targets) |
| `/proc/diskstats` | per-device read/write sector counts |
| `/proc/meminfo` | total/free/available/cached/buffers |
| `/proc/stat` | cpu user/nice/system/idle/iowait/irq/softirq |
| `/proc/loadavg` | 1/5/15 minute load averages |

### Two-Snapshot Delta Model

A single snapshot has no CPU rate information (only cumulative tick counts). The engine takes two snapshots separated by `interval_seconds` and computes:

```
cpu_usage_pct = (Δutime + Δstime) × 100 / (clk_tck × interval_seconds)
read_rate_kb  = Δread_bytes  / interval_seconds / 1024
write_rate_kb = Δwrite_bytes / interval_seconds / 1024
```

---

## 7. AI Reasoning Layer

### 7.1 Providers

| Provider | Protocol | Streaming |
|---|---|---|
| **Gemini** | HTTPS REST, SSE (`?alt=sse`) | ✅ Server-Sent Events |
| **Ollama** | HTTP REST (`/api/chat`) | ✅ NDJSON stream |

### 7.2 System Prompt

The system prompt is embedded at compile time in `src/ai.cpp`:

```
You are the SysPilot Operating System Reasoning Agent...
Answer:
  1. What happened inside the machine?
  2. Why did it happen?
  3. Which process/thread triggered it?
  4. How did effects propagate through kernel subsystems?
  5. What was the performance impact?
  6. What specific changes will resolve the root cause?
```

### 7.3 Context Payload Structure

```json
{
  "telemetry": {
    "system":   { "load_avg": "...", "mem_total_kb": 0, ... },
    "process":  { "pid": 0, "name": "", "state": "", ... },
    "ebpf":     "optional bpftrace trace log",
    "execution_profile": { "top_symbols": [...], "call_graph": "..." }
  },
  "causal_chain": {
    "symptom_node": "pid:4582",
    "nodes": [...],
    "edges": [...]
  },
  "codebase_context": "...relevant source excerpts from vector search...",
  "last_command": { "command": "...", "exit_code": 1 }
}
```

### 7.4 MdStreamer — Real-Time Markdown Renderer

`src/ui/streamer.cpp` implements a streaming state machine that converts Markdown tokens to ANSI escape sequences as bytes arrive from the HTTP stream:

| Markdown | ANSI Output |
|---|---|
| `**bold**` | `\x1b[1m...\x1b[0m` |
| `` `code` `` | `\x1b[96m...\x1b[0m` |
| ` ```block``` ` | `\x1b[90m▌\x1b[36m...` |
| `# Header` | `\x1b[1;33m...\x1b[0m` |
| `- item` | `\x1b[36m•\x1b[0m ...` |

---

## 8. Terminal UI (TUI)

The TUI is implemented in `src/ui/tui.cpp` using **raw `termios` mode** — no ncurses, no external TUI library.

### Event Loop Architecture

```
enable_raw_mode()          ← termios: ICANON=0, ECHO=0, VMIN=1, VTIME=0
┌─────────────────────────────────────────────────────┐
│  Every 250ms (or on keypress):                      │
│                                                     │
│  1. get_processes()      ← reads /proc via robin_map │
│  2. sort by CPU% / I/O / PID                        │
│  3. draw_border()        ← repeat_utf8() box chars  │
│  4. print_at(row, col)   ← ANSI cursor positioning  │
│  5. fd_set select(100ms) ← non-blocking key read    │
│  6. Handle keypress:                                │
│     'q'      → restore_raw_mode() + exit           │
│     Tab      → cycle sort_column                   │
│     'e'/↵    → suspend TUI → run CausalTrace+AI    │
│     's'      → kill(pid, SIGSTOP)                  │
│     'r'      → kill(pid, SIGCONT)                  │
│     'k'      → kill(pid, SIGKILL)                  │
└─────────────────────────────────────────────────────┘
restore_raw_mode()
```

### Process Data Pipeline

```
/proc directory scan (every 250ms)
    │
    └─► TuiProcess {
            pid, ppid, state,
            cpu_usage_pct,
            read_rate_kb, write_rate_kb,
            is_anomalous  ← state=="D" || cpu>80% || write>5000 KB/s
        }
    │
    └─► tsl::robin_map for O(1) PID lookup between frames
```

---

## 9. Vector Codebase Index

`src/codebase.cpp` implements a local vector database for mapping causal graph nodes (processes, file paths) back to source code.

### Indexing Pipeline

```
workspace files
    │ chunk_file() — AST-aware chunking (function boundaries, class blocks)
    │
    └─► RawChunk {content, start_line, end_line}
        │ HTTP POST → Gemini embedding API / Ollama /api/embeddings
        │
        └─► float[] embedding (768 or 1536 dimensions)
            │ normalize_vector()
            │
            └─► VectorDb {file_path, content, embedding}
                └─► saved to ~/.syspilot/<workspace_hash>.vdb (binary format v2)
```

### SIMD Cosine Similarity

The similarity search dispatches at compile time based on CPU capability:

```cpp
#if defined(__AVX2__)
    // 8 floats/clock via _mm256_fmadd_ps (FMA)
    // Horizontal reduction with _mm256_extractf128_ps
    // ~0.8 µs for 1536-dim vectors

#elif defined(__SSE4_1__)
    // 4 floats/clock via _mm_add_ps + _mm_mul_ps
    // ~1.6 µs for 1536-dim vectors

#else
    // Scalar fallback
    // ~4.6 µs for 1536-dim vectors
#endif
```

With `-march=native`, the compiler selects the best available variant automatically.

---

## 10. HPC Library Stack

| Library | Role | Integration Point |
|---|---|---|
| **mimalloc** | Global `new`/`delete`/`malloc`/`free` override | `#include <mimalloc-new-delete.h>` first in `main.cpp` |
| **simdjson ondemand** | SIMD JSON parsing (3–4 GB/s) | Daemon socket requests + causal engine daemon responses |
| **{fmt}** | Zero-alloc JSON response building | `fmt::memory_buffer` in all daemon socket handlers |
| **Intel TBB** | `concurrent_hash_map` — bucket-level locking | Daemon process tree |
| **Moodycamel ConcurrentQueue** | Lock-free MPMC event buffer | Daemon event ring |
| **tsl::robin_map** | Open-addressing hash map | Causal engine node store + proc snapshots |
| **tsl::robin_set** | Open-addressing hash set | BFS `visited` set + path sets in exports |
| **spdlog (async)** | Non-blocking structured logging | Daemon all log calls |
| **AVX2 SIMD** | FMA cosine similarity | `codebase.cpp` vector search |

### Library Selection Rationale

**Why mimalloc over tcmalloc/jemalloc?**
- Lower worst-case latency (no background compaction pauses)
- Better small-object performance (the dominant allocation pattern here)
- Drop-in without any code changes via `mimalloc-new-delete.h`

**Why simdjson ondemand over RapidJSON?**
- Zero intermediate DOM tree allocation — streams through JSON once
- `string_view` results point directly into the input buffer (no copies)
- SIMD validation and structural character scanning

**Why tsl::robin_map over absl::flat_hash_map?**
- Header-only, no Abseil build dependency
- Comparable performance to Abseil on string_view keys
- Robin-hood backward-shift deletion avoids tombstone buildup

**Why Moodycamel over boost::lockfree::queue?**
- MPMC (multiple producers, multiple consumers) by design
- No allocation on enqueue (uses pre-allocated node pool)
- Stronger progress guarantees than Boost's algorithm

---

## 11. Data Flow — End to End

```
User: ./syspilot explain --pid 4582 --causal

main.cpp
  ├─ query UNIX socket → syspilotd (if running)
  │   └─ simdjson parse response → robin_map<pid, GraphNode>
  │
  ├─ CausalGraph::build_graph(interval=2, ebpf=false, target=4582)
  │   ├─ take_proc_snapshot() × 2  [sleep 2s between]
  │   ├─ add_node() for each PID   [StringArena bump alloc]
  │   ├─ add_edge() SPAWNED_BY     [arena.alloc(from), arena.alloc(to)]
  │   ├─ open fd scan → resource nodes
  │   ├─ disk delta → I/O rate calculation
  │   └─ contention detection → CONTENDS_WITH edges
  │
  ├─ trace_root_cause("pid:4582")
  │   └─ BFS with tsl::robin_set<string_view>
  │       → path: ["pid:4582", "resource:/dev/sda", "pid:1337"]
  │
  ├─ serialize_chain_to_json(path)
  │   └─ JSON string with nodes + edges + rates
  │
  ├─ telemetry::collect_process_telemetry(4582)
  ├─ profiler::profile_process(4582, deep=false)
  ├─ codebase::query_context(workspace, "pid:4582 /dev/sda")
  │   └─ cosine_similarity (AVX2) over VectorDb chunks
  │
  ├─ Build final prompt JSON context
  │
  └─ ai::query_ai_stream(config, prompt, streamer)
      ├─ libcurl HTTPS POST → Gemini SSE endpoint
      └─ MdStreamer::push_chunk() per SSE data event
          └─ ANSI-rendered output to stdout
```

---

## 12. Security Model

### What SysPilot reads (safe)
- `/proc/[pid]/stat`, `/proc/[pid]/io`, `/proc/[pid]/fd/` — read-only
- `/proc/diskstats`, `/proc/meminfo`, `/proc/stat` — read-only
- `~/.syspilot/context.log` — written by the shell hook
- Local source files — read-only for vector indexing

### What SysPilot does NOT do
- Does not write to any system files
- Does not execute user-supplied strings in a shell (enforced by `src/safety.cpp`)
- Does not read `/etc/shadow`, `/proc/[pid]/mem`, or any credential files
- Does not send raw terminal output to the AI (only structured JSON)

### Command safety allowlist (`src/safety.cpp`)

All tools that execute system commands go through `safety.cpp`, which maintains:
- An explicit **allowlist** of safe read-only commands (`ls`, `git status`, `df`, etc.)
- A **denylist** of destructive patterns (`rm`, `mkfs`, `dd`, `chmod`, etc.)
- Static analysis of the command string before any `fork()`/`exec()`

### UNIX socket permissions
The daemon socket at `/tmp/syspilot.sock` is created with `chmod 0666` — world-readable so non-root users can query it. Only the daemon process writes to the process tree; clients are read-only consumers.

---

## 13. Performance Targets

| Operation | Target Latency | Achieved Via |
|---|---|---|
| Process event (fork → in-memory) | < 500 µs | Netlink push, zero copy, TBB |
| Daemon socket response (process_tree) | < 2 ms | simdjson + fmt memory_buffer |
| CausalTrace graph construction | < 3 s (2s interval) | StringArena + robin_map |
| BFS root-cause traversal | < 500 µs | tsl::robin_set + compact queue |
| TUI frame render | < 5 ms | Raw ANSI, no ncurses |
| Vector cosine search (1000 chunks) | < 5 ms | AVX2 FMA SIMD |
| Global heap alloc latency | < 60 ns | mimalloc thread-local heaps |
