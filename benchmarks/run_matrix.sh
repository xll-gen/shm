#!/bin/bash
set -e

# Build everything
task build

cleanup() {
    pkill -f "server -w" || true
    rm -f /dev/shm/SimpleIPC* || true
}

run_test() {
    THREADS=$1
    SIZE=$2
    DURATION=10

    echo "============================================================"
    echo "Running Test: Threads=$THREADS, Size=$SIZE"
    echo "============================================================"

    cleanup

    # Start Server (Go)
    # Note: Go server doesn't take -s size, it just echoes whatever it gets.
    # It parses headers (8 bytes) + payload.
    # It adapts to slot size dynamically from shared memory.
    ./benchmarks/go/server -w $THREADS > server_temp.log 2>&1 &
    SERVER_PID=$!

    sleep 2

    # Start Client (C++)
    ./build/benchmarks/shm_benchmark -t $THREADS -s $SIZE -d $DURATION

    # Cleanup
    kill $SERVER_PID || true
    wait $SERVER_PID 2>/dev/null || true
    cleanup
    echo ""
}

# Run Parameter Matrix
# Standard sizes
run_test 1 64
run_test 2 64
run_test 4 64
run_test 8 64

# Larger sizes
run_test 1 1024
run_test 4 1024

# Extreme threads (Oversubscription)
run_test 16 64
