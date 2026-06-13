#!/bin/bash
set -e

echo "🛠️ Compiling SysPilot Operating System Reasoning Agent..."

g++ -std=c++17 -O3 -Wall -Wextra \
  src/main.cpp \
  src/utils.cpp \
  src/safety.cpp \
  src/config.cpp \
  src/telemetry.cpp \
  src/profiler.cpp \
  src/codebase.cpp \
  src/causal_engine.cpp \
  src/ai.cpp \
  src/ui/streamer.cpp \
  src/install.cpp \
  -o syspilot

echo "✅ Compilation successful! Created 'syspilot' executable."
