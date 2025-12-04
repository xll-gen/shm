#!/bin/bash
set -e

echo "Compiling C++ Host..."
g++ -O3 -pthread experiments/pingpong/main.cpp -o experiments/pingpong/host

echo "Compiling Go Guest..."
go build -o experiments/pingpong/guest experiments/pingpong/main.go

run_test() {
    THREADS=$1
    echo "========================================"
    echo "Running with $THREADS threads..."

    # Run Guest in background
    ./experiments/pingpong/guest $THREADS &
    GUEST_PID=$!

    # Run Host
    ./experiments/pingpong/host $THREADS

    # Cleanup
    kill $GUEST_PID 2>/dev/null || true
    wait $GUEST_PID 2>/dev/null || true
}

run_test 1
run_test 2

# Final cleanup of binaries (as requested)
echo "Cleaning up binaries..."
rm -f experiments/pingpong/host experiments/pingpong/guest
