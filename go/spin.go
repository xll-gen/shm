package shm

import (
	"runtime"
	"sync/atomic"
)

// WaitStrategy implements an adaptive spin-wait strategy.
type WaitStrategy struct {
	CurrentLimit int32
	MinSpin      int32
	MaxSpin      int32
	IncStep      int32
	DecStep      int32
}

// NewWaitStrategy creates a new WaitStrategy with default optimized values.
func NewWaitStrategy() *WaitStrategy {
	return &WaitStrategy{
		CurrentLimit: 2000,
		MinSpin:      100,
		MaxSpin:      20000,
		IncStep:      200,
		DecStep:      100,
	}
}

// Wait executes the adaptive wait logic.
//
// condition: A function that returns true if the wait condition is met.
// sleepAction: A function to execute when spinning fails (e.g. wait on semaphore).
//
// Returns true if the condition was met.
func (w *WaitStrategy) Wait(condition func() bool, sleepAction func()) bool {
	ready := false
	limit := int(atomic.LoadInt32(&w.CurrentLimit))

	// 1. Spin Phase
	for i := 0; i < limit; i++ {
		if condition() {
			ready = true
			break
		}
		// Yield less frequently to reduce scheduler overhead (every 64 iterations)
		if i&0x3F == 0 {
			runtime.Gosched()
		}
	}

	if ready {
		// Success: Reward
		if limit < int(w.MaxSpin) {
			newLimit := limit + int(w.IncStep)
			if newLimit > int(w.MaxSpin) {
				newLimit = int(w.MaxSpin)
			}
			atomic.StoreInt32(&w.CurrentLimit, int32(newLimit))
		}
	} else {
		// Failure: Punish
		if limit > int(w.MinSpin) {
			newLimit := limit - int(w.DecStep)
			if newLimit < int(w.MinSpin) {
				newLimit = int(w.MinSpin)
			}
			atomic.StoreInt32(&w.CurrentLimit, int32(newLimit))
		}

		// 2. Sleep Phase
		sleepAction()

		// Check again
		if condition() {
			ready = true
		}
	}

	return ready
}
