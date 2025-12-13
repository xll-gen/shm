#!/bin/bash
set -e

# Path to the Aeron Media Driver (aeronmd)
AERONMD="../../benchmarks/external/aeron/cppbuild/binaries/aeronmd"
# Path to the benchmark binary
BINARY="./build/pingpong_aeron"

if [ ! -f "$BINARY" ]; then
    echo "Benchmark binary not found at $BINARY"
    exit 1
fi

if [ ! -f "$AERONMD" ]; then
    echo "Aeron Media Driver not found at $AERONMD"
    exit 1
fi

THREADS=1
if [ -n "$1" ]; then
    THREADS=$1
fi

# Start Aeron Media Driver
echo "Starting Aeron Media Driver..."
$AERONMD > aeronmd.log 2>&1 &
AERONMD_PID=$!

# Give it a moment to start
sleep 2

# Cleanup function to kill background processes
cleanup() {
    echo "Cleaning up..."
    kill $PONG_PID 2>/dev/null || true
    kill $AERONMD_PID 2>/dev/null || true
    wait $AERONMD_PID 2>/dev/null || true
}
trap cleanup EXIT

# Start Pong in background
echo "Starting Pong (Echo Server)..."
$BINARY -m pong -t $THREADS &
PONG_PID=$!

# Give Pong a moment to initialize and connect
sleep 2

# Start Ping (Benchmark)
echo "Starting Ping (Benchmark)..."
$BINARY -m ping -t $THREADS
