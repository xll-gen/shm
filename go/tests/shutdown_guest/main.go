package main
import (
	"log"
	"os"
	"os/signal"
	"syscall"
	"time"
	shm "github.com/xll-gen/shm/go"
)
func main() {
	log.SetFlags(log.LstdFlags | log.Lmicroseconds)
	name := "DirectShutdownTestShm"
	// 1. Create Guest
	g, err := shm.NewDirectGuest(name)
	if err != nil {
		// It might fail if host hasn't created SHM yet. Retry.
		for i := 0; i < 20; i++ {
			time.Sleep(500 * time.Millisecond)
			g, err = shm.NewDirectGuest(name)
			if err == nil {
				break
			}
		}
		if err != nil {
			log.Fatalf("Failed to join shm: %v", err)
		}
	}
	defer g.Close()
	// 2. Start
	ready := make(chan struct{})
	g.Start(func(req []byte, resp []byte) int {
		log.Println("Guest: Received Request. Sleeping 2s...")
		time.Sleep(2 * time.Second)
		log.Println("Guest: Woke up. Copying data.")
		copy(resp, req)
		return len(req)
	}, ready)
	<-ready
	log.Println("Guest: Started.")
	// Wait for signal
	sigs := make(chan os.Signal, 1)
	signal.Notify(sigs, syscall.SIGINT, syscall.SIGTERM)
    // Removed stdin check to prevent premature exit in background mode
	<-sigs
	log.Println("Guest: Exiting.")
}
