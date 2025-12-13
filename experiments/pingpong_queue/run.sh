#!/bin/bash
set -e

# Compile C++ Host
echo "Compiling C++ Host..."
g++ -O3 -pthread -o host main.cpp -lrt

# Compile Go Guest
echo "Compiling Go Guest..."
cd go
go build -o guest main.go
cd ..

# Run
echo "Starting Guest in background..."
./go/guest &
GUEST_PID=$!

# Give guest a moment to initialize/wait
sleep 1

echo "Starting Host..."
./host

# Cleanup
echo "Cleaning up..."
kill $GUEST_PID 2>/dev/null || true
wait $GUEST_PID 2>/dev/null || true
rm -f host go/guest
