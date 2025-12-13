#!/bin/bash
set -e

# This script assumes 'pingpong_aeron' binary is in the current directory or build/
# and that an Aeron Media Driver is running or available.

BINARY="./build/pingpong_aeron"
if [ ! -f "$BINARY" ]; then
    BINARY="./pingpong_aeron"
fi

if [ ! -f "$BINARY" ]; then
    echo "Binary not found. Please build first."
    exit 1
fi

THREADS=1
if [ -n "$1" ]; then
    THREADS=$1
fi

# Start Pong in background
echo "Starting Pong (Echo Server)..."
$BINARY -m pong -t $THREADS &
PONG_PID=$!

sleep 1

# Start Ping (Benchmark)
echo "Starting Ping (Benchmark)..."
$BINARY -m ping -t $THREADS

# Cleanup
echo "Cleaning up..."
kill $PONG_PID
wait $PONG_PID 2>/dev/null
echo "Done."
