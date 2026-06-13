# 🤖 SysPilot

**SysPilot** is an intelligent CLI terminal assistant and operating system reasoning agent built in C++. It monitors your terminal activity, gathers low-level telemetry, and uses AI providers (Gemini, Ollama) to explain failed commands, diagnose resource contention, and help you debug Linux and programming issues instantly.

It features **CausalTrace**, a directed multigraph reasoning engine that performs real-time causal dependency tracing directly from kernel and `/proc` telemetry.

---

## 🚀 Features
- **CausalTrace Observability Engine:** Construct a directed multigraph of processes and resources (files, sockets, block devices). Traces bottlenecks, dirty page cache saturation, and Virtual File System (VFS) locks using a reverse-BFS root-cause traversal.
- **Context-Aware Explanations:** Automatically captures the last command you ran and its exit code.
- **Safe Read-Only Tools:** Gathers environment data (like `pwd`, `ls`, Git status) automatically to give the AI context while maintaining strict safety guardrails.
- **Real-time Streaming:** Answers stream directly into your terminal in real-time.
- **Terminal-Native Markdown:** Compiles Markdown on-the-fly into bold text and formatted code blocks directly in your shell.

---

## 📦 Installation

### 1. Prerequisites
You will need a Linux environment with a C++17 compatible compiler (`g++` or `clang++`), standard build utilities, and the `/proc` filesystem.

### 2. Build & Install
Clone the repository and compile using the provided build script:
```bash
git clone https://github.com/yourusername/syspilot.git
cd syspilot
./build.sh
```

### 3. Setup SysPilot
Initialize SysPilot to generate the config files and terminal hook script:
```bash
./syspilot install
```

### 4. Configure your AI Provider
SysPilot supports multiple AI providers (Gemini, Ollama). By default, it uses Gemini. 
Configure your API keys using the CLI:
```bash
syspilot config set-key gemini YOUR_GEMINI_API_KEY
```

To switch to a local model via Ollama:
```bash
syspilot provider ollama
syspilot config set-url ollama http://localhost:11434
```

### 5. Enable Terminal Hook
SysPilot hooks into your terminal to track command history and exit codes. 
Add the following line to your `~/.bashrc` (or `~/.zshrc`):
```bash
source ~/.syspilot/syspilot.sh
```
Then restart your terminal or run `source ~/.bashrc`.

---

## 💡 Usage

SysPilot has three main operational modes:

### `syspilot explain`
Did a command just fail? Ask SysPilot to explain it. It will automatically read your `context.log` to find the last command you ran and tell you why it failed.
```bash
$ cat non_existent_file.txt
cat: non_existent_file.txt: No such file or directory

$ syspilot explain
```

### Causal Diagnostics: `syspilot explain --pid <PID> --causal`
Diagnose execution stalls, resource blockages, and I/O contention using the **CausalTrace** engine. It builds a dependency graph, finds anomalous processes, and traces the exact causal chain:
```bash
$ syspilot explain --pid 4582 --causal
```
*Note: Use `--no-index` if you want to bypass local codebase context mapping.*

### `syspilot ask "<question>"`
Ask a general technical question or debug your current environment.
```bash
$ syspilot ask "how do I configure vm.dirty_ratio in sysctl.conf?"
```

You can optionally pass `--deep` to gather deeper profiling context (such as CPU/memory stack traces and active `perf` symbols):
```bash
$ syspilot explain --deep
```

---

## 🛠️ Status & Uninstallation
Check if the hook is properly loaded:
```bash
syspilot status
```

To remove SysPilot from your system completely:
```bash
syspilot uninstall
```
*(Don't forget to remove the `source ~/.syspilot/syspilot.sh` line from your `~/.bashrc`)*
