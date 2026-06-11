package shm

import (
	"sync"
	"testing"
	"time"
)

// TestSetTimeout_NoRaceWithConcurrentRead exercises SetTimeout against the
// hot-path reader concurrently. Before responseTimeout was made atomic this
// was a data race (plain int64 field written by SetTimeout while
// SendGuestCall read it). Run under -race; a non-atomic field trips the
// detector here.
func TestSetTimeout_NoRaceWithConcurrentRead(t *testing.T) {
	g := &DirectGuest{}
	g.SetTimeout(10 * time.Second)

	var wg sync.WaitGroup
	const iters = 5000

	// Writer: mutate the timeout repeatedly.
	wg.Add(1)
	go func() {
		defer wg.Done()
		for i := 0; i < iters; i++ {
			g.SetTimeout(time.Duration(i+1) * time.Millisecond)
		}
	}()

	// Readers: read it via the same accessor the send path uses.
	for r := 0; r < 4; r++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for i := 0; i < iters; i++ {
				if g.responseTimeout() <= 0 {
					t.Errorf("responseTimeout returned non-positive value")
					return
				}
			}
		}()
	}
	wg.Wait()
}

// TestClose_Idempotent verifies Close can be called multiple times (and
// concurrently) without panicking. closeOnce guards the unmap/handle-close so
// the body runs at most once. A zero-value DirectGuest has no real handles, so
// the underlying CloseShm/CloseEvent are no-ops, but double-invocation must
// still be safe.
func TestClose_Idempotent(t *testing.T) {
	g := &DirectGuest{name: "test-idempotent-close"}

	// Sequential double close.
	g.Close()
	g.Close()

	// Concurrent close on a fresh instance.
	g2 := &DirectGuest{name: "test-idempotent-close-concurrent"}
	var wg sync.WaitGroup
	for i := 0; i < 8; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			g2.Close()
		}()
	}
	wg.Wait()
}
