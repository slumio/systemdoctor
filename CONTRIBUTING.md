# Contributing to SysPilot

Welcome to SysPilot! This document provides an overview of the architecture and instructions for developers who want to contribute.

## 🏗️ Architecture Overview

The codebase is structured to be modular and is implemented in C++17.

- **`src/main.cpp`**: CLI entry point. Handles argument parsing (`--causal`, `--deep`, `--no-index`), config setup, and routes commands.
- **`src/causal_engine.cpp` & `src/causal_engine.h`**: The **CausalTrace** reasoning engine. Operates by constructing a directed multigraph of processes and active system resources (files, sockets, block devices). Features:
  - **Two-Pass Resource Mapping**: Filters out idle inherited file descriptors to keep the graph high-signal.
  - **Reverse BFS Traversal**: Starts at the symptom process and traverses back to active disturbers.
- **`src/telemetry.cpp` & `src/telemetry.h`**: Handles low-level OS telemetry gathering by parsing `/proc/stat`, `/proc/[pid]/stat`, `/proc/[pid]/fd`, and `/proc/diskstats`.
- **`src/install.cpp` & `src/install.h`**: Handles CLI configuration initialization and shell hook setup under `~/.syspilot/`.
- **`src/codebase.cpp` & `src/codebase.h`**: Maps anomalous processes and files identified in the causal graph back to local files or codebase definitions.
- **`src/profiler.cpp` & `src/profiler.h`**: Gathers thread stack traces from `/proc/[pid]/task/[tid]/stack` and handles 1.5-second `perf` CPU sampling if `--deep` is enabled.
- **`src/ai.cpp` & `src/ai.h`**: The AI API interaction layer. Interfaces with Gemini (via cURL) and local Ollama instances. Contains a custom `MdStreamer` to render Markdown formatting into ANSI terminal colors in real time.
- **`src/safety.cpp` & `src/safety.h`**: Validates commands against a denylist to ensure safe read-only system context gathering.
- **`src/config.cpp` & `src/config.h`**: Handles parsing and serialization of the JSON configuration files (`~/.syspilot/config.json`).
- **`src/utils.cpp` & `src/utils.h`**: Utility functions for string parsing, trimming, file existence, and running shell commands.

## 🔧 Development Setup

1. **Clone the repository:**
   ```bash
   git clone https://github.com/yourusername/syspilot.git
   cd syspilot
   ```

2. **Build the project:**
   Compile the source files using the build script:
   ```bash
   ./build.sh
   ```

3. **Run Contention Tests:**
   You can verify CausalTrace's behavior using the contention simulation:
   ```bash
   # Run the contention script to generate mock processes and invoke causal diagnostics
   bash /home/joyboy/.gemini/antigravity/brain/86de7233-9661-48db-ab30-3f8b1c46da8d/scratch/test_contention.sh
   ```

## 🛠️ How to Add a New Telemetry Metric

1. **Add telemetry function**: In `src/telemetry.h`, declare your metric parser (e.g., `uint64_t get_net_recv_bytes()`).
2. **Implement in C++**: Parse the relevant file (e.g. `/proc/net/dev`) in `src/telemetry.cpp`.
3. **Connect to Graph Node**: If the metric is process-specific or resource-specific, update `GraphNode` in `src/causal_engine.h` and populate it inside `CausalGraph::take_proc_snapshot()`.

## 🔏 Safety First
SysPilot executes OS commands to gather system context. **Never** allow dynamic user input to be passed directly into a shell execution. Always use utility runners that pass through the check constraints in `src/safety.cpp`. 

If adding context tools, ensure they are strictly read-only and non-destructive!
