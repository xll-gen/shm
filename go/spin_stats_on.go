//go:build shm_benchstats

package shm

import "sync/atomic"

// Benchmark-only counter writes. Compiled in only under `-tags shm_benchstats`
// (used by benchmarks/go); the production build gets the no-op variants in
// spin_stats_off.go so the Direct-Exchange hot path performs zero atomic adds.
// The counters themselves are declared unconditionally in spin.go.

func recordSpinSuccess()   { atomic.AddUint64(&WaitStatsSpinSuccess, 1) }
func recordSleepFallback() { atomic.AddUint64(&WaitStatsSleepFallback, 1) }
func recordIters(n uint64) { atomic.AddUint64(&WaitStatsIterCount, n) }
