#!/bin/bash
set -e

echo "Compiling C++ Host..."
g++ -O3 -march=native -flto -funroll-loops -pthread experiments/pingpong/main.cpp -o experiments/pingpong/host

echo "Compiling Go Guest..."
cd experiments/pingpong/go && go build -o ../guest main.go && cd ../../..

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

if [ -n "$1" ]; then
    run_test $1
else
    run_test 1
    run_test 2
    run_test 3
fi

# Final cleanup of binaries (as requested)
echo "Cleaning up binaries..."
rm -f experiments/pingpong/host experiments/pingpong/guest
