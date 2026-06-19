package shm

import (
	"runtime"
	"sync/atomic"
)

// waitStrategyAsmChunk bounds the iteration count handed to spinUntilEq32 in
// one burst. Between bursts the Go wrapper calls runtime.Gosched(), preserving
// the legacy 4096-iter yield cadence without putting Gosched into the asm
// hot loop.
const waitStrategyAsmChunk uintptr = 4096

// Tunables for the single adaptive WaitStrategy.
// These match EXPERIMENTS.md "Exp 3" and are intentionally not configurable —
// one strategy is used by every Direct Exchange slot, host and guest alike.
// If a new workload demands different tuning, change the constants here,
// re-run benchmarks/harness.ps1, and update EXPERIMENTS.md.
const (
	// Spin window is sized to bridge the typical Direct Exchange inter-request
	// gap on a non-oversubscribed system. kMaxSpin × per-iter cost (~2–5 ns on
	// x86 once the closure-callsite is factored in) gives a ~100–250 µs ceiling
	// before falling back to OS-wait. IncStep ≫ DecStep so a single failed spin
	// doesn't collapse the window. Gosched is throttled to every 4096 iters —
	// every-128 burns ~5 µs of Gosched per call at high spin counts and breaks
	// short-timeout callers.
	waitStrategyMinSpin   int32 = 100
	waitStrategyMaxSpin   int32 = 50000
	waitStrategyIncStep   int32 = 5000
	waitStrategyDecStep   int32 = 1000
	waitStrategyYieldMask int   = 0xFFF // runtime.Gosched() every 4096 spin iterations
)

// Benchmark instrumentation: package-global counters measuring how often
// Wait/WaitState resolve within the spin window vs. fall through to the
// OS-wait path. Consumed by benchmarks/go/main.go to report spin efficiency
// when tuning the adaptive constants above.
//
// The counters are always declared (so the benchmark module compiles without
// build tags), but the post-wait recordSpinSuccess/recordSleepFallback/
// recordIters writes are no-ops unless the package is built with
// `-tags shm_benchstats` (see spin_stats_on.go / spin_stats_off.go). The
// production Direct-Exchange hot path therefore performs zero atomic adds on
// these three adjacent (single-cache-line) counters; the benchmark harness
// passes the tag to get real numbers.
var (
	WaitStatsSpinSuccess   uint64
	WaitStatsSleepFallback uint64
	WaitStatsIterCount     uint64
)

// WaitStrategy is the single adaptive spin/sleep strategy used by Direct
// Exchange slots. It exposes only CurrentLimit for diagnostics; do not poke at
// it from outside the wait loop.
type WaitStrategy struct {
	CurrentLimit int32
}

// NewWaitStrategy returns a freshly-initialised strategy ready for Wait calls.
func NewWaitStrategy() *WaitStrategy {
	return &WaitStrategy{CurrentLimit: waitStrategyMaxSpin}
}

// Wait spins on condition up to the current adaptive limit, periodically yielding
// the Go scheduler. If the condition does not become true within the spin window
// it punishes the limit and calls sleepAction (typically a semaphore wait), then
// re-checks once before returning.
//
// Returns true if the condition was met.
func (w *WaitStrategy) Wait(condition func() bool, sleepAction func()) bool {
	ready := false
	limit := int(atomic.LoadInt32(&w.CurrentLimit))

	iters := 0
	for i := 0; i < limit; i++ {
		iters = i
		if condition() {
			ready = true
			break
		}
		cpuPause()
		// Skip i==0 so the fast path (condition becomes true within the first
		// 127 iterations) never pays Gosched overhead. After that, yield every
		// 4096 iterations to keep the Go scheduler responsive in oversubscribed
		// environments without excessive Gosched overhead.
		if i > 0 && i&waitStrategyYieldMask == 0 {
			runtime.Gosched()
		}
	}
	recordIters(uint64(iters))

	if ready {
		recordSpinSuccess()
		if limit < int(waitStrategyMaxSpin) {
			newLimit := limit + int(waitStrategyIncStep)
			if newLimit > int(waitStrategyMaxSpin) {
				newLimit = int(waitStrategyMaxSpin)
			}
			atomic.StoreInt32(&w.CurrentLimit, int32(newLimit))
		}
	} else {
		recordSleepFallback()
		if limit > int(waitStrategyMinSpin) {
			newLimit := limit - int(waitStrategyDecStep)
			if newLimit < int(waitStrategyMinSpin) {
				newLimit = int(waitStrategyMinSpin)
			}
			atomic.StoreInt32(&w.CurrentLimit, int32(newLimit))
		}

		sleepAction()

		if condition() {
			ready = true
		}
	}

	return ready
}

// WaitState is the assembly-driven fast path for the common case
// `atomic.LoadUint32(addr) == want`. It is semantically equivalent to:
//
//	w.Wait(func() bool { return atomic.LoadUint32(addr) == want }, sleepAction)
//
// but the inner spin loop runs entirely inside spin_amd64.s — no per-iteration
// Go closure CALL, no cpuPause() CALL, no TLS reload. Burst length is bounded
// by waitStrategyAsmChunk so runtime.Gosched() can fire on the same 4096-iter
// cadence the closure-based Wait uses.
//
// All three production Direct-Exchange callsites (direct.go, guest_slot.go)
// match this shape and should call WaitState instead of Wait.
func (w *WaitStrategy) WaitState(addr *uint32, want uint32, sleepAction func()) bool {
	limit := uintptr(atomic.LoadInt32(&w.CurrentLimit))
	var totalIters uintptr
	ready := false

	for totalIters < limit {
		chunk := limit - totalIters
		if chunk > waitStrategyAsmChunk {
			chunk = waitStrategyAsmChunk
		}
		iters, ok := spinUntilEq32(addr, want, chunk)
		totalIters += iters
		if ok {
			ready = true
			break
		}
		if totalIters < limit {
			runtime.Gosched()
		}
	}
	recordIters(uint64(totalIters))

	if ready {
		recordSpinSuccess()
		if int32(limit) < waitStrategyMaxSpin {
			newLimit := int32(limit) + waitStrategyIncStep
			if newLimit > waitStrategyMaxSpin {
				newLimit = waitStrategyMaxSpin
			}
			atomic.StoreInt32(&w.CurrentLimit, newLimit)
		}
		return true
	}

	recordSleepFallback()
	if int32(limit) > waitStrategyMinSpin {
		newLimit := int32(limit) - waitStrategyDecStep
		if newLimit < waitStrategyMinSpin {
			newLimit = waitStrategyMinSpin
		}
		atomic.StoreInt32(&w.CurrentLimit, newLimit)
	}

	sleepAction()

	if atomic.LoadUint32(addr) == want {
		return true
	}
	return false
}
