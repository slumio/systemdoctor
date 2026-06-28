#!/bin/bash
set -e

echo "🛠️  Compiling SysPilot — High-Performance System Intelligence Suite"
echo "     Libraries: mimalloc · simdjson · spdlog · fmt · TBB · tsl::robin_map · ConcurrentQueue"

# ── Compiler & Standard ─────────────────────────────────────────────────────
CXX=${CXX:-g++}
STD="-std=c++17"

# ── Optimization Flags ───────────────────────────────────────────────────────
OPT="-Ofast -flto -march=native -fomit-frame-pointer -funroll-loops"
OPT="$OPT -fno-plt -ffast-math"          # No PLT stubs; relaxed FP
OPT="$OPT -DNDEBUG"                       # Strip all asserts

# ── Warning flags ────────────────────────────────────────────────────────────
WARN="-Wall -Wextra -Wno-unused-parameter"

# ── Include paths ────────────────────────────────────────────────────────────
INCLUDES="-Isrc -Isrc/vendor"

# ── Source files ─────────────────────────────────────────────────────────────
SOURCES="
  src/main.cpp
  src/utils.cpp
  src/safety.cpp
  src/config.cpp
  src/telemetry.cpp
  src/profiler.cpp
  src/codebase.cpp
  src/causal_engine.cpp
  src/ai.cpp
  src/ui/streamer.cpp
  src/ui/tui.cpp
  src/daemon.cpp
  src/install.cpp
"

# ── Link libraries ───────────────────────────────────────────────────────────
LIBS="-pthread"
LIBS="$LIBS -lmimalloc"          # Microsoft mimalloc — global allocator
LIBS="$LIBS -lsimdjson"          # SIMD-accelerated JSON (AVX2/SSE4)
LIBS="$LIBS -lspdlog -lfmt"      # spdlog (async logging) + {fmt}
LIBS="$LIBS -ltbb"               # Intel oneTBB (concurrent_hash_map)
LIBS="$LIBS /lib/x86_64-linux-gnu/libcurl.so.4"   # HTTP for AI API

# ── Build ─────────────────────────────────────────────────────────────────────
$CXX $STD $OPT $WARN $INCLUDES $SOURCES -o syspilot $LIBS

echo "✅  syspilot built — all HPC libraries linked."
echo ""
echo "  Run:  ./syspilot daemon &   # start background telemetry daemon"
echo "        ./syspilot monitor    # launch live TUI"
