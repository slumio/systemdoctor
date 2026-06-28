# 🤖 SysPilot

> **High-performance Operating System Reasoning Agent** — real-time causal diagnostics, microsecond-latency telemetry, AI-powered root-cause analysis, and a zero-dependency terminal UI.

<div align="center">

![Language](https://img.shields.io/badge/language-C%2B%2B17-blue?style=for-the-badge&logo=cplusplus)
![Platform](https://img.shields.io/badge/platform-Linux%20x86__64-orange?style=for-the-badge&logo=linux)
![License](https://img.shields.io/badge/license-MIT-green?style=for-the-badge)
![Build](https://img.shields.io/badge/build-passing-brightgreen?style=for-the-badge)

**mimalloc · simdjson · Intel TBB · Moodycamel · tsl::robin_map · AVX2 SIMD · spdlog · {fmt}**

</div>

---

## What is SysPilot?

SysPilot is a **systems-level diagnostic suite** for Linux that combines three things in one binary:

1. **`syspilotd` Daemon** — A zero-polling background service that subscribes to the Linux kernel's Netlink Process Connector (`cn_proc`) to receive process lifecycle events (fork, exec, exit) in real time. It maintains a lock-free, in-memory process tree and exposes it over a UNIX domain socket at sub-100µs latency.

2. **CausalTrace Engine** — A directed multigraph reasoning engine that constructs a dependency graph of processes and system resources (files, sockets, block devices, pipes) from two time-stamped `/proc` snapshots. It performs a reverse-BFS traversal to trace observable symptoms (high I/O wait, zombie processes, mutex contention) back to their root causes.

3. **AI Reasoning Layer** — Serializes the causal chain to a structured JSON context payload and submits it to Gemini or a local Ollama instance to generate human-readable, technically precise root-cause reports with actionable remediation steps.

---

## ✨ Features

| Feature | Description |
|---|---|
| **Zero-polling telemetry** | Netlink `cn_proc` events — kernel pushes process events instead of polling `/proc` |
| **CausalTrace BFS** | Reverse-BFS multigraph traversal from symptom to root cause |
| **AI diagnostics** | Gemini & Ollama integration with real-time streaming responses |
| **Live TUI monitor** | Raw ANSI terminal UI — process list, anomaly highlighting, signal controls |
| **eBPF tracing** | Optional `bpftrace` syscall tracing for open/connect/execve events |
| **Vector codebase index** | AVX2 SIMD cosine similarity search maps causal graph nodes to source files |
| **Terminal markdown** | Custom streaming ANSI renderer for bold, code blocks, and color |
| **mimalloc global allocator** | Thread-local heaps; 40–70% faster allocation vs glibc |

---

## 📦 Installation

### Prerequisites

| Dependency | Version | Purpose |
|---|---|---|
| `g++` or `clang++` | C++17 | Compiler |
| `libmimalloc-dev` | ≥ 2.1 | Global heap allocator |
| `libsimdjson-dev` | ≥ 3.6 | SIMD JSON parsing |
| `libspdlog-dev` | ≥ 1.12 | Async structured logging |
| `libfmt-dev` | ≥ 9.1 | Zero-alloc string formatting |
| `libtbb-dev` | ≥ 2021.11 | Concurrent hash map |
| `libcurl4` | any | HTTP for AI API calls |

Install all on Ubuntu/Debian:
```bash
sudo apt-get install -y \
  build-essential g++ \
  libmimalloc-dev libsimdjson-dev \
  libspdlog-dev libfmt-dev libtbb-dev \
  libcurl4-openssl-dev
```

### Build

```bash
git clone https://github.com/yourusername/syspilot.git
cd syspilot
chmod +x build.sh
./build.sh
```

The build script compiles with `-Ofast -flto -march=native` and links all HPC libraries. Expected output:

```
🛠️  Compiling SysPilot — High-Performance System Intelligence Suite
     Libraries: mimalloc · simdjson · spdlog · fmt · TBB · tsl::robin_map · ConcurrentQueue
✅  syspilot built — all HPC libraries linked.
```

### Install

```bash
./syspilot install
```

This creates `~/.syspilot/` with:
- `config.json` — provider settings, API keys, model selection
- `syspilot.sh` — shell hook for capturing command history and exit codes

Add to your `~/.bashrc` or `~/.zshrc`:
```bash
source ~/.syspilot/syspilot.sh
```

---

## 🚀 Usage

### Start the Daemon
```bash
./syspilot daemon &
```
The daemon subscribes to Netlink `cn_proc`, initializes an in-memory process tree, and listens on `/tmp/syspilot.sock`. It has near-zero CPU usage — no polling.

### Live TUI Monitor
```bash
./syspilot monitor
```

| Key | Action |
|---|---|
| `Tab` | Cycle sort: CPU% → I/O Rate → PID |
| `↑` / `↓` or `j` / `k` | Navigate process list |
| `e` or `Enter` | AI root-cause explanation for selected process |
| `s` | Send `SIGSTOP` (suspend) |
| `r` | Send `SIGCONT` (resume) |
| `k` | Send `SIGKILL` (terminate) |
| `q` | Quit |

### Explain Last Failed Command
```bash
./syspilot explain
```

### Causal Diagnostic by PID
```bash
# Standard procfs snapshot
./syspilot explain --pid 4582 --causal

# With eBPF syscall tracing (requires root or CAP_BPF)
sudo ./syspilot explain --pid 4582 --causal --ebpf

# With deep perf CPU profiling
./syspilot explain --pid 4582 --causal --deep
```

### Ask a General Question
```bash
./syspilot ask "why is vm.dirty_ratio causing write stalls under my workload?"
```

### Configure AI Provider
```bash
# Gemini
./syspilot config set-key gemini YOUR_API_KEY

# Local Ollama
./syspilot provider ollama
./syspilot config set-url ollama http://localhost:11434
```

### Check Status / Uninstall
```bash
./syspilot status
./syspilot uninstall
```

---

## 🏗️ Architecture (Brief)

```
Linux Kernel (cn_proc)
      │ Netlink push events (fork/exec/exit)
      ▼
syspilotd daemon
  ├─ concurrent_hash_map<pid, ProcessNode>   (Intel TBB)
  ├─ ConcurrentQueue<ProcessEventRecord>     (Moodycamel, lock-free)
  └─ UNIX socket /tmp/syspilot.sock          (simdjson in, fmt out)
      │
      ▼
CausalTrace Engine
  ├─ take_proc_snapshot() → tsl::robin_map<pid, GraphNode>
  ├─ build_graph() → directed multigraph
  └─ trace_root_cause() → reverse-BFS with tsl::robin_set
      │
      ▼
AI Layer (Gemini / Ollama)
  └─ streaming JSON → MdStreamer → ANSI terminal
```

See [ARCHITECTURE.md](ARCHITECTURE.md) for the full deep-dive.

---

## 📁 Repository Structure

```
syspilot/
├── build.sh                  # Build script (-Ofast -flto -march=native)
├── src/
│   ├── main.cpp              # CLI entry point & command router
│   ├── daemon.cpp/h          # syspilotd: Netlink + UNIX socket server
│   ├── causal_engine.cpp/h   # CausalTrace: multigraph + BFS + export
│   ├── telemetry.cpp/h       # /proc parser & system snapshot collector
│   ├── ai.cpp/h              # Gemini/Ollama API + MdStreamer renderer
│   ├── codebase.cpp/h        # Vector DB + SIMD cosine similarity search
│   ├── profiler.cpp/h        # perf CPU profiler integration
│   ├── config.cpp/h          # JSON config read/write (~/.syspilot/)
│   ├── safety.cpp/h          # Command safety allowlist
│   ├── utils.cpp/h           # String, file, shell utilities
│   ├── install.cpp/h         # Shell hook installer
│   ├── ui/
│   │   ├── tui.cpp/h         # Raw ANSI terminal UI (no ncurses)
│   │   └── streamer.cpp/h    # Real-time Markdown→ANSI renderer
│   ├── vendor/
│   │   ├── concurrentqueue.h  # Moodycamel ConcurrentQueue (vendored)
│   │   └── tsl/               # tsl::robin_map / robin_set (vendored)
│   └── nlohmann/              # nlohmann/json (vendored)
├── ARCHITECTURE.md
├── developer_guide.md
└── CONTRIBUTING.md
```

---

## 📄 License

MIT — see [LICENSE](LICENSE).
