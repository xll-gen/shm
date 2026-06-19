//go:build !shm_benchstats

package shm

// Production no-op spin-stat recorders. The benchmark-only atomic-add variants
// live in spin_stats_on.go (`-tags shm_benchstats`). These are inlined away,
// so Wait/WaitState perform zero atomic adds on the WaitStats counters in the
// shipped build.

func recordSpinSuccess()   {}
func recordSleepFallback() {}
func recordIters(uint64)   {}
