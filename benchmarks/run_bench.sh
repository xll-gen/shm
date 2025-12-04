#!/bin/bash
set -e

# Cleanup function
cleanup() {
    echo "Cleaning up..."
    pkill -f "benchmarks/go/server" || true
    rm -f /dev/shm/SimpleIPC*
    rm -f /dev/shm/sem.SimpleIPC*
}
trap cleanup EXIT

echo "Building C++..."
mkdir -p benchmarks/build
cd benchmarks/build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
cd ../..

echo "Building Go..."
cd benchmarks/go
go build -o server main.go
cd ../..

run_case() {
    THREADS=$1
    echo "------------------------------------------------"
    echo "Running with $THREADS threads..."

    # Explicit cleanup before run
    rm -f /dev/shm/SimpleIPC*
    rm -f /dev/shm/sem.SimpleIPC*
    rm -f server_${THREADS}.log
    pkill -f "benchmarks/go/server" || true

    # Start Server
    ./benchmarks/go/server -w $THREADS > server_${THREADS}.log 2>&1 &
    SERVER_PID=$!

    # Wait for server to be ready
    sleep 1

    # Run Client with 60s timeout
    set +e
    timeout 60s ./benchmarks/build/shm_benchmark -t $THREADS
    RET=$?
    set -e

    if [ $RET -eq 124 ]; then
        echo "ERROR: Benchmark TIMED OUT!"
        cat server_${THREADS}.log
    elif [ $RET -ne 0 ]; then
        echo "ERROR: Benchmark Failed with code $RET"
        cat server_${THREADS}.log
    fi

    kill $SERVER_PID 2>/dev/null || true
    wait $SERVER_PID 2>/dev/null || true
}

# Run cases
run_case 1
# run_case 2
# run_case 4
