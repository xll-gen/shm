// Package main implements the Benchmark Guest Server.
//
// It connects to the shared memory "SimpleIPC" and echoes back any data it receives.
// This is used in conjunction with the C++ host benchmark.
package main

import (
	"flag"
	"fmt"
	"os"
	"os/signal"
	"syscall"

	"github.com/xll-gen/shm/go"
)

var (
	// workers defines the number of worker goroutines to spawn.
	// In Direct Mode, this should match the number of Host slots.
	workers   = flag.Int("w", 1, "Number of workers")
	guestCall = flag.Bool("guest-call", false, "Enable guest call scenario")
)

// main is the entry point for the Go Benchmark Guest.
func main() {
	flag.Parse()

	// runtime.GOMAXPROCS(*workers + 2) // +2 for runtime/gc

	fmt.Printf("Starting Server with %d workers...\n", *workers)
	if *guestCall {
		fmt.Println("Guest Call scenario enabled.")
	}

	client, err := shm.Connect("SimpleIPC")
	if err != nil {
		fmt.Printf("Failed to connect: %v\n", err)
		os.Exit(1)
	}
	defer client.Close()

	// Zero-copy Echo Handler
	client.Handle(func(req []byte, respBuf []byte, msgId uint32) int32 {
		if *guestCall {
			// Trigger a Guest Call to the Host
			callData := []byte("callback")
			_, err := client.SendGuestCall(callData, shm.MsgIdGuestCall)
			if err != nil {
				// Log error but continue to reply to original request
				//fmt.Printf("Guest call failed: %v\n", err)
			}
		}

		// Simple Echo
		n := copy(respBuf, req)
		return int32(n)
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
