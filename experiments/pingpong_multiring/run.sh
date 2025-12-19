#!/bin/bash
set -e

# Compile C++ Host
g++ -O3 -march=native -pthread -o host main.cpp -lrt

# Compile Go Guest
cd go
go build -o guest main.go
cd ..

# Run Benchmark
echo "--- Running 1 Thread ---"
./go/guest -w 1 &
GUEST_PID=$!
sleep 1
./host 1
kill $GUEST_PID 2>/dev/null || true
wait $GUEST_PID 2>/dev/null || true

echo ""
echo "--- Running 4 Threads ---"
./go/guest -w 4 &
GUEST_PID=$!
sleep 1
./host 4
kill $GUEST_PID 2>/dev/null || true
wait $GUEST_PID 2>/dev/null || true

echo ""
echo "--- Running 8 Threads ---"
./go/guest -w 8 &
GUEST_PID=$!
sleep 1
./host 8
kill $GUEST_PID 2>/dev/null || true
wait $GUEST_PID 2>/dev/null || true

# Cleanup
rm -f /dev/shm/pingpong_multiring_shm
