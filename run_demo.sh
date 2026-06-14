#!/bin/bash
set -e

# Setup test workspace
cd /home/joyboy/Systempilot/syspilot

echo "=================================================="
echo "🚀 SysPilot CausalTrace: Live Contention Demo"
echo "=================================================="

# Ensure project is compiled
echo "🛠️  Compiling SysPilot..."
./build.sh > /dev/null
echo "✓ Compilation successful."

# Clean up previous run
rm -f ./language_server ./mock_db /tmp/syspilot_shared.log /tmp/syspilot_demo.pid

# Create shared resource
touch /tmp/syspilot_shared.log

# Copy sleep to mock_db
cp /bin/sleep ./mock_db

echo "1. Launching parent simulation (language_server)..."
# Write a Python simulator for language_server
cat << 'EOF' > ./language_server
#!/usr/bin/env python3
import os
import sys
import time
import ctypes

# Set process name to 'language_server' in kernel space
try:
    libc = ctypes.CDLL('libc.so.6')
    libc.prctl(15, b'language_server', 0, 0, 0)
except Exception:
    pass

# Open shared log file on FD 3 without close-on-exec flag
fd = os.open('/tmp/syspilot_shared.log', os.O_WRONLY | os.O_CREAT | os.O_TRUNC)
os.dup2(fd, 3)

# Fork child process
pid = os.fork()
if pid == 0:
    # Child process: exec mock_db
    # This inherits FD 3 silently
    try:
        os.execv('./mock_db', ['./mock_db', '120'])
    except Exception as e:
        sys.stderr.write(f"Exec failed: {e}\n")
        sys.exit(1)
else:
    # Parent process (language_server)
    # Save the child PID so the demo runner can find it
    with open('/tmp/syspilot_demo.pid', 'w') as f:
        f.write(f"DB_PID={pid}\n")
        
    # Write heavily to FD 3 in a loop (~10 MB/s)
    data = b'A' * 1024 * 1024 # 1 MB
    try:
        while True:
            os.write(3, data)
            time.sleep(0.1)
    except KeyboardInterrupt:
        pass
    except Exception:
        pass
EOF

chmod +x ./language_server

# Run language_server simulation in background
./language_server &
LANG_PID=$!

# Wait for PID file to be written
sleep 1.5
DB_PID=$(cat /tmp/syspilot_demo.pid | cut -d'=' -f2)

echo ""
echo "📊 Simulated System Hierarchy Launched:"
echo "   ├── [language_server]   (PID: $LANG_PID) - Leaks FD 3 & performs heavy I/O writes"
echo "   └── [mock_db]           (PID: $DB_PID)   - Child database inheriting FD 3"
echo "   Shared resource: /tmp/syspilot_shared.log"
echo "=================================================="
echo ""
echo "🔍 Running SysPilot CausalTrace diagnostics..."
echo "Command: ./syspilot explain --pid $DB_PID --causal --no-index"
echo ""

# Run SysPilot explain
./syspilot explain --pid $DB_PID --causal --no-index

echo ""
echo "=================================================="
echo "🧹 Cleaning up background processes..."
kill -9 $LANG_PID $DB_PID 2>/dev/null || true
rm -f ./language_server ./mock_db /tmp/syspilot_shared.log /tmp/syspilot_demo.pid
echo "✅ Demo complete."
