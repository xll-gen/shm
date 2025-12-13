#!/bin/bash
set -e

# Update paths to point to the current directory (pingpong_coordinator)
BASE_DIR="experiments/pingpong_coordinator"

echo "Compiling C++ Host..."
g++ -O3 -pthread $BASE_DIR/main.cpp -o $BASE_DIR/host

echo "Compiling Go Guest..."
cd $BASE_DIR/go && go build -o ../guest main.go && cd ../../..

run_test() {
    THREADS=$1
    echo "========================================"
    echo "Running with $THREADS threads..."

    # Run Guest in background
    ./$BASE_DIR/guest $THREADS &
    GUEST_PID=$!

    # Run Host
    ./$BASE_DIR/host $THREADS

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

# Final cleanup of binaries
echo "Cleaning up binaries..."
rm -f $BASE_DIR/host $BASE_DIR/guest
