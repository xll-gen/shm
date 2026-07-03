//go:build !shm_benchstats

package shm

// Production no-op spin-stat recorders and getters. The benchmark-only
// atomic-add variants and the strategy registry live in spin_stats_on.go
// (`-tags shm_benchstats`). These are inlined away, so Wait/WaitState perform
// zero atomic adds on the WaitStats counters in the shipped build and
// NewWaitStrategy retains nothing.

func registerWaitStats(*WaitStrategy) {}

func (w *WaitStrategy) recordSpinSuccess()   {}
func (w *WaitStrategy) recordSleepFallback() {}
func (w *WaitStrategy) recordIters(uint64)   {}

// WaitStatsSpinSuccess returns 0 in builds without `-tags shm_benchstats`.
func WaitStatsSpinSuccess() uint64 { return 0 }

// WaitStatsSleepFallback returns 0 in builds without `-tags shm_benchstats`.
func WaitStatsSleepFallback() uint64 { return 0 }

// WaitStatsIterCount returns 0 in builds without `-tags shm_benchstats`.
func WaitStatsIterCount() uint64 { return 0 }
