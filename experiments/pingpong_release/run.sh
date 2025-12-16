#!/bin/bash
set -e

echo "Compiling C++ Host (Release/Acquire)..."
g++ -O3 -pthread experiments/pingpong_release/main.cpp -o experiments/pingpong_release/host

echo "Compiling Go Guest..."
cd experiments/pingpong_release/go && go build -o ../guest main.go && cd ../../..

run_test() {
    THREADS=$1
    echo "========================================"
    echo "Running with $THREADS threads..."

    # Run Guest in background
    ./experiments/pingpong_release/guest $THREADS &
    GUEST_PID=$!

    # Run Host
    ./experiments/pingpong_release/host $THREADS

    # Cleanup
    kill $GUEST_PID 2>/dev/null || true
    wait $GUEST_PID 2>/dev/null || true
}

if [ -n "$1" ]; then
    run_test $1
else
    run_test 1
    run_test 2
    run_test 3
fi

# Final cleanup of binaries (as requested)
echo "Cleaning up binaries..."
rm -f experiments/pingpong_release/host experiments/pingpong_release/guest
