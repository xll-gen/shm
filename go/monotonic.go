package shm

import "time"

// MonotonicNanos returns wall-clock nanoseconds since Unix epoch.
//
// Used by the v0.7.0 SlotHeader.Lease field — slot owners stamp their
// activity time, future versions reclaim slots whose lease is too old.
//
// We deliberately use wall-clock (not Go's monotonic clock) so the value
// is comparable across processes AND across languages: C++ side stores
// GetSystemTimeAsFileTime (coarse system tick, ~0.5-15.6 ms — see
// Platform::MonotonicNanos in include/shm/Platform.h and the resolution
// note in SPECIFICATION.md §3.6) and Go stores time.Now().UnixNano().
// Both sit in the same Unix-epoch timeline; the resolution asymmetry is
// bounded by one tick and reclaim thresholds are seconds-scale.
//
// NOT strictly monotonic — an NTP step can move the clock backward.
// Acceptable for lease purposes: a backward step causes a spurious
// reclamation candidate (the CAS guard in v0.7.1 re-checks state); a
// forward step delays reclamation. Neither corrupts data.
//
// The function name is kept "MonotonicNanos" to match the C++ side's
// public symbol. Revisit naming when v0.7.1 lands the reclamation API.
func MonotonicNanos() uint64 {
	return uint64(time.Now().UnixNano())
}
