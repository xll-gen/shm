//go:build shm_benchstats

package shm

import (
	"sync"
	"sync/atomic"
)

// Benchmark-only counter writes and aggregation. Compiled in only under
// `-tags shm_benchstats` (used by benchmarks/go); the production build gets
// the no-op variants in spin_stats_off.go so the Direct-Exchange hot path
// performs zero atomic adds.
//
// The counters live value-embedded in each WaitStrategy (see spin.go) so the
// per-RTT LOCK XADDs land on the waiting core's own cache line instead of a
// shared global — the aggregate getters below sum over every strategy
// NewWaitStrategy has registered. Registered strategies are retained for
// process lifetime, which is acceptable for benchmark builds only.

var waitStatsRegistry struct {
	mu         sync.Mutex
	strategies []*WaitStrategy
}

func registerWaitStats(w *WaitStrategy) {
	waitStatsRegistry.mu.Lock()
	waitStatsRegistry.strategies = append(waitStatsRegistry.strategies, w)
	waitStatsRegistry.mu.Unlock()
}

func (w *WaitStrategy) recordSpinSuccess()   { atomic.AddUint64(&w.statSpinSuccess, 1) }
func (w *WaitStrategy) recordSleepFallback() { atomic.AddUint64(&w.statSleepFallback, 1) }
func (w *WaitStrategy) recordIters(n uint64) { atomic.AddUint64(&w.statIterCount, n) }

func sumWaitStats(field func(*WaitStrategy) *uint64) uint64 {
	waitStatsRegistry.mu.Lock()
	defer waitStatsRegistry.mu.Unlock()
	var sum uint64
	for _, w := range waitStatsRegistry.strategies {
		sum += atomic.LoadUint64(field(w))
	}
	return sum
}

// WaitStatsSpinSuccess returns the count of spin loops that resolved within
// the adaptive limit (i.e. the condition became true before the OS-wait
// fallback fired), summed over all registered strategies.
func WaitStatsSpinSuccess() uint64 {
	return sumWaitStats(func(w *WaitStrategy) *uint64 { return &w.statSpinSuccess })
}

// WaitStatsSleepFallback returns the count of waits that exhausted the spin
// window and fell through to the OS-wait sleepAction.
func WaitStatsSleepFallback() uint64 {
	return sumWaitStats(func(w *WaitStrategy) *uint64 { return &w.statSleepFallback })
}

// WaitStatsIterCount returns the total spin iterations accumulated across all
// resolved waits. Combined with WaitStatsSpinSuccess gives AvgItersPerSpin.
func WaitStatsIterCount() uint64 {
	return sumWaitStats(func(w *WaitStrategy) *uint64 { return &w.statIterCount })
}
