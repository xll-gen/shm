package shm

import (
	"runtime"
	"sync/atomic"
	"unsafe"
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
//
// NOTE: a time-based-calibration variant of these constants (measure per-iter
// PAUSE cost at init, express the window in wall-clock time) was prototyped and
// benchmarked on an 8-LP VM (see IMPROVEMENT_BACKLOG.md "스핀 전략 튜닝"). A
// ~200µs target regressed throughput up to −65%; a ~3ms target merely tied
// these fixed constants. Net null/negative on the only host tested, so the
// fixed iteration counts stand.
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

// Benchmark instrumentation: per-WaitStrategy counters measuring how often
// Wait/WaitState resolve within the spin window vs. fall through to the
// OS-wait path. Consumed by benchmarks/go/main.go (via the aggregate
// WaitStats* getters) to report spin efficiency when tuning the adaptive
// constants above.
//
// The counter fields are always declared on WaitStrategy (so the benchmark
// module compiles without build tags and a bare `&WaitStrategy{}` literal
// works with zero registration), but the post-wait recordSpinSuccess/
// recordSleepFallback/recordIters writes — and the registry feeding the
// aggregate getters — exist only under `-tags shm_benchstats` (see
// spin_stats_on.go / spin_stats_off.go). The production Direct-Exchange hot
// path therefore performs zero atomic adds on these counters.
//
// 2026-07-03 per-strategy sharding: previously the counters were three
// package-global cache-line-padded cells (v0.7.12 layout). Under
// `-tags shm_benchstats` every RTT still fired two LOCK XADDs that all
// worker goroutines funnelled onto the same two global lines — with pinned
// workers spread across CCXs that is a cross-CCX line ping-pong on every
// round trip, the guest-side analog of the 2026-07-03 C++ bench stats fix.
// Each slot owns exactly one WaitStrategy with at most one active waiter
// (ActiveWait gate), so per-strategy fields keep the incremented line local
// to the pinned core; the aggregate getters sum over a tag-gated registry
// populated by NewWaitStrategy.

// WaitStrategy is the single adaptive spin/sleep strategy used by Direct
// Exchange slots. It exposes only CurrentLimit for diagnostics; do not poke at
// it from outside the wait loop.
//
// The struct is padded to exactly one 64-byte cache line so each
// heap-allocated strategy (Go size class 64, 64-byte aligned) owns its line —
// adjacent strategies must not share a line or the benchstats sharding above
// degrades back into false sharing.
type WaitStrategy struct {
	CurrentLimit int32
	_            [4]byte

	// Benchmark-only spin counters, value-embedded (never pointers) so an
	// unregistered zero-value strategy still works. Written only under
	// `-tags shm_benchstats`; always zero otherwise.
	statSpinSuccess   uint64
	statSleepFallback uint64
	statIterCount     uint64

	_ [32]byte // pad struct to 64 bytes (one cache line)
}

// Compile-time assertion: WaitStrategy is exactly one 64-byte cache line.
var (
	_ [unsafe.Sizeof(WaitStrategy{}) - 64]byte
	_ [64 - unsafe.Sizeof(WaitStrategy{})]byte
)

// NewWaitStrategy returns a freshly-initialised strategy ready for Wait calls.
// Under `-tags shm_benchstats` it also registers the strategy so the aggregate
// WaitStats* getters can sum its counters (registered strategies are retained
// for process lifetime — benchmark builds only).
func NewWaitStrategy() *WaitStrategy {
	w := &WaitStrategy{CurrentLimit: waitStrategyMaxSpin}
	registerWaitStats(w)
	return w
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
	w.recordIters(uint64(iters))

	if ready {
		w.recordOutcome(true, int32(limit))
	} else {
		w.recordOutcome(false, int32(limit))
		sleepAction()
		if condition() {
			ready = true
		}
	}

	return ready
}

// recordOutcome folds the post-wait bookkeeping shared by Wait and WaitState:
// it records the spin/sleep counters (no-ops without -tags shm_benchstats) and
// adapts CurrentLimit — widening by IncStep on a spin-resolved wait (hit) or
// narrowing by DecStep when the wait fell through to sleepAction (miss), each
// clamped to [waitStrategyMinSpin, waitStrategyMaxSpin]. limit is the snapshot
// the wait loop ran with. Callers invoke sleepAction themselves after a miss so
// the counter/limit update is published before the (blocking) OS wait.
func (w *WaitStrategy) recordOutcome(hit bool, limit int32) {
	if hit {
		w.recordSpinSuccess()
		if limit < waitStrategyMaxSpin {
			newLimit := limit + waitStrategyIncStep
			if newLimit > waitStrategyMaxSpin {
				newLimit = waitStrategyMaxSpin
			}
			atomic.StoreInt32(&w.CurrentLimit, newLimit)
		}
		return
	}
	w.recordSleepFallback()
	if limit > waitStrategyMinSpin {
		newLimit := limit - waitStrategyDecStep
		if newLimit < waitStrategyMinSpin {
			newLimit = waitStrategyMinSpin
		}
		atomic.StoreInt32(&w.CurrentLimit, newLimit)
	}
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
	w.recordIters(uint64(totalIters))

	if ready {
		w.recordOutcome(true, int32(limit))
		return true
	}

	w.recordOutcome(false, int32(limit))
	sleepAction()

	if atomic.LoadUint32(addr) == want {
		return true
	}
	return false
}
