#!/bin/bash
set -e

# Cleanup function
cleanup() {
    pkill -f "server" || true
    rm -f /dev/shm/SimpleIPC* || true
}

# Initial cleanup
cleanup

# Build C++
echo "[Build] C++ Host..."
mkdir -p benchmarks/build
cd benchmarks/build
cmake .. -DCMAKE_BUILD_TYPE=Release > /dev/null
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
    echo "Running Case: $THREADS Threads"
    echo "----------------------------------------"

    cleanup

    # Start Server
    ./benchmarks/go/server -w $THREADS > server_${THREADS}.log 2>&1 &
    SERVER_PID=$!

    sleep 1

    # Run Client with Timeout
    # Use timeout to prevent hangs. Normal run is ~1s.
    # To enable verbose logging (every 100 ops), set VERBOSE=1
    ARGS="-t $THREADS"
    if [ "$VERBOSE" == "1" ]; then
        ARGS="$ARGS -v"
    fi

    if timeout 60s ./benchmarks/build/shm_benchmark $ARGS; then
        echo "Success."
    else
        RET=$?
        if [ $RET -eq 124 ]; then
            echo "TIMEOUT (60s)!"
        else
            echo "FAILED with $RET"
        fi
    fi

    kill $SERVER_PID || true
    wait $SERVER_PID || true
}

run_case 1
run_case 4
run_case 8

cleanup
echo "Done."
