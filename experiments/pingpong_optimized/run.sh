#!/bin/bash
set -e

# Compile C++ Host
echo "Compiling C++ Host (Optimized)..."
g++ -O3 -pthread -o host main.cpp -lrt

# Compile Go Guest
echo "Compiling Go Guest (Optimized)..."
cd go
go build -o guest main.go
cd ..

# Run
echo "Running Benchmark..."
./host 4 &
HOST_PID=$!

sleep 0.5
./go/guest 4 &
GUEST_PID=$!

wait $HOST_PID
wait $GUEST_PID
