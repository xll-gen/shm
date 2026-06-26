//go:build shm_benchstats

package shm

import "sync/atomic"

// Benchmark-only counter writes. Compiled in only under `-tags shm_benchstats`
// (used by benchmarks/go); the production build gets the no-op variants in
// spin_stats_off.go so the Direct-Exchange hot path performs zero atomic adds.
// The counter storage is declared (cache-line padded) in spin.go.

func recordSpinSuccess()   { atomic.AddUint64(&waitStatsSpinSuccess.v, 1) }
func recordSleepFallback() { atomic.AddUint64(&waitStatsSleepFallback.v, 1) }
func recordIters(n uint64) { atomic.AddUint64(&waitStatsIterCount.v, n) }
