package main

import (
	"flag"
	"fmt"
	"os"
	"os/signal"
	"runtime"
	"syscall"

	"github.com/xll-gen/shm/go"
)

var (
	workers = flag.Int("w", 1, "Number of workers")
)

func main() {
	flag.Parse()

	runtime.GOMAXPROCS(*workers + 2) // +2 for runtime/gc

	fmt.Printf("Starting Server with %d workers...\n", *workers)

	client, err := shm.Connect("SimpleIPC")
	if err != nil {
		fmt.Printf("Failed to connect: %v\n", err)
		os.Exit(1)
	}
	defer client.Close()

	// Zero-copy Echo Handler
	client.Handle(func(req []byte, respBuf []byte) uint32 {
		// Simple Echo
		n := copy(respBuf, req)
		return uint32(n)
	})

	fmt.Println("Server ready.")

	// Handle Ctrl+C
	c := make(chan os.Signal, 1)
	signal.Notify(c, os.Interrupt, syscall.SIGTERM)

	go func() {
		<-c
		fmt.Println("\nShutting down...")
		client.Close()
		os.Exit(0)
	}()

	client.Start()
	client.Wait()
}
