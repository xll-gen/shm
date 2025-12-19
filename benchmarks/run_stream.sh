#!/bin/bash
set -e

# Ensure we are in the repo root
cd "$(dirname "$0")/.."

# Clean previous build
rm -rf build

# Build C++ Benchmark (Release)
mkdir -p build
cd build
cmake .. -DSHM_BUILD_BENCHMARKS=ON -DCMAKE_BUILD_TYPE=Release
make -j shm_benchmark
cd ..

# Build Go Server
cd benchmarks/go
go build -o server .
cd ../..

run_benchmark() {
    SIZE=$1
    CHUNK=$2
    THREADS=$3
    DURATION=$4

    echo "----------------------------------------------------------------"
    echo "Running Stream Benchmark: Size=$SIZE, Chunk=$CHUNK, Threads=$THREADS"
    echo "----------------------------------------------------------------"

    # Cleanup previous runs
    rm -f /dev/shm/test_stream
    rm -f /dev/shm/sem.test_stream*

    # Start Host
    ./build/benchmarks/shm_benchmark --name /test_stream --stream -s $SIZE -c $CHUNK -t $THREADS -d $DURATION > host.log 2>&1 &
    HOST_PID=$!

    # Give Host time to init SHM
    sleep 1

    # Start Go Server
    ./benchmarks/go/server --name /test_stream --stream -w $THREADS > server.log 2>&1 &
    GO_PID=$!

    # Wait for Host to finish
    wait $HOST_PID

    # Cat results
    cat host.log

    # Cleanup
    kill $GO_PID 2>/dev/null || true
    wait $GO_PID 2>/dev/null || true
}

# 1KB Stream (Sanity)
run_benchmark 1024 256 1 2

# 128MB Stream
# 128 * 1024 * 1024 = 134217728
run_benchmark 134217728 65536 1 5
