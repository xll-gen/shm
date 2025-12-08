#!/bin/bash
set -e

# Cleanup function
cleanup() {
    pkill -f "server" || true
    rm -f /dev/shm/SimpleIPC* || true
}

# Initial cleanup
cleanup

# Build C++ with Profiling
echo "[Build] C++ Host (Profiling Enabled)..."
mkdir -p benchmarks/build
cd benchmarks/build
cmake .. -DCMAKE_BUILD_TYPE=Release -DENABLE_PROFILING=ON > /dev/null
make -j$(nproc) > /dev/null
cd ../..

# Build Go
echo "[Build] Go Guest..."
cd benchmarks/go
go build -o server main.go
cd ../..

run_case() {
    THREADS=$1
    echo "----------------------------------------"
    echo "Running Profiling Case: $THREADS Threads"
    echo "----------------------------------------"

    cleanup

    # Start Server with Profiling
    ./benchmarks/go/server -w $THREADS -cpuprofile cpu_${THREADS}.prof > server_${THREADS}.log 2>&1 &
    SERVER_PID=$!

    sleep 1

    # Run Client with Timeout
    ARGS="-t $THREADS"
    if timeout 60s ./benchmarks/build/shm_benchmark $ARGS; then
        echo "Benchmark Success."
    else
        echo "Benchmark TIMEOUT or FAILED"
    fi

    # Graceful shutdown to flush Go profile
    kill -SIGINT $SERVER_PID || true
    wait $SERVER_PID || true

    # Rename C++ profile (gmon.out)
    if [ -f gmon.out ]; then
        mv gmon.out gmon_${THREADS}.out
        echo "Saved gmon_${THREADS}.out"
    fi

    echo "Saved cpu_${THREADS}.prof"
}

run_case 1
run_case 4
run_case 8

cleanup
echo "Profiling Done."
