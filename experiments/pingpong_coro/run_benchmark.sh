#!/bin/bash
set -e

# Cleanup function
cleanup() {
    echo "[Wrapper] Cleaning up..."
    pkill -f pingpong_coro || true
    pkill -f server_new || true
    rm -f /dev/shm/pingpong_shm
    rm -f /dev/shm/sem.pp_*
}
trap cleanup EXIT

cleanup

THREADS=3

# Start Host in background
echo "[Wrapper] Starting Host with $THREADS threads..."
./build/pingpong_coro $THREADS &
HOST_PID=$!

# Wait for Host to initialize SHM
echo "[Wrapper] Waiting for SHM..."
for i in {1..50}; do
    if [ -f /dev/shm/pingpong_shm ]; then
        echo "[Wrapper] SHM found!"
        break
    fi
    sleep 0.1
done

# Start Guest in foreground (with timeout)
echo "[Wrapper] Starting Guest with $THREADS threads..."
timeout 30s ./go/server_new $THREADS

# Wait for Host to finish
echo "[Wrapper] Waiting for Host to exit..."
wait $HOST_PID
