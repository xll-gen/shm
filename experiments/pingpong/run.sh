#!/bin/bash
set -e

echo "Compiling C++ Host..."
g++ -O3 -pthread experiments/pingpong/main.cpp -o experiments/pingpong/host

echo "Compiling Go Guest..."
go build -o experiments/pingpong/guest experiments/pingpong/main.go

echo "Running..."
# Run Guest in background
./experiments/pingpong/guest &
GUEST_PID=$!

# Give guest a moment to start polling (though it waits for file, so order is loose)
# Actually host creates file, so host should start first?
# Wait, Host creates file. Guest loops waiting for file.
# So we must start Host.
# BUT Host loops waiting for Guest?
# In my Host code: "Waiting for guest... sleep(1)".
# So Host creates file, sleeps 1s, then starts.
# Guest waits for file, opens it, then loops.
# So:
# 1. Start Guest (it will fail to open file for a bit)
# 2. Start Host (it creates file)
# OR
# 1. Start Host in BG?
# Host runs logic. If I start Host in FG, I can't start Guest easily unless Host forks?
# Let's run Guest in BG. It retries open for 5s (50 * 100ms).
# Host starts, creates file. Guest finds it.

./experiments/pingpong/host

# Cleanup
kill $GUEST_PID 2>/dev/null || true
