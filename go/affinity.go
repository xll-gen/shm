package shm

import (
	"runtime"
	"sync"
)

// AffinityMode selects how worker goroutines bind themselves to physical CPU
// resources. The zero value (AffinityAuto) applies CCX-aware pinning
// automatically when the host topology benefits — chiplet CPUs (Ryzen /
// Threadripper / Epyc) and multi-socket Xeon — and is a no-op on
// monolithic-L3 systems (most single-socket Intel desktops, single-CCX
// Zen parts, constrained VMs). Set AffinityNone for explicit opt-out.
//
// On chiplet CPUs the dominant 64-byte ping-pong cost between host and
// guest is the cross-CCD cache-line bounce (~80-100ns) traversing
// Infinity Fabric. Pinning the host worker thread and guest worker
// goroutine for the same slot index to the SAME CCX (shared L3) reduces
// that to ~30-50ns intra-CCX coherency. On Ryzen 9 3900X measurements
// showed +25-57% multi-thread throughput. On monolithic-L3 Intel single-
// socket systems Auto becomes a no-op (CcxMasks() returns one mask
// covering all LPs, which has no coherency benefit), so callers see no
// regression.
//
// SMT-sibling collision risk: AffinityLocal pins to a whole CCX mask (3+
// physical cores × 2 SMT siblings), so the OS scheduler chooses within the
// mask. PAUSE-every-iter in the spin loop handles the same-physical-core
// case gracefully. AffinityDedicated would pin to a single LP and risks the
// 2026-EXPERIMENTS.md §Exp 5 starvation pattern unless SMT-sibling exclusion
// is layered in — it is not implemented in this opt-in trial.
type AffinityMode int

const (
	// AffinityAuto — default (zero value). Apply AffinityLocal pinning
	// when the host topology reports >1 shared-L3 group (chiplet CPUs,
	// multi-socket Xeon); fall through to no pinning on monolithic-L3
	// systems where LockOSThread + same-as-all-LPs mask would add
	// runtime overhead without coherency benefit. New in v0.8.1: pre-
	// v0.8.1 the zero value was AffinityNone-equivalent; this default-
	// enable is backward-compatible on Intel single-socket hosts
	// (no-op) and unlocks the chiplet win on AMD by default.
	AffinityAuto AffinityMode = 0
	// AffinityNone — explicit opt-out; no pinning regardless of
	// topology. Use when the caller wants to manage thread placement
	// itself (e.g. via SetProcessAffinityMask before spawning).
	AffinityNone AffinityMode = 1
	// AffinityLocal — force CCX-mask pinning even on monolithic-L3
	// systems. On a single-CCX host this still calls LockOSThread +
	// SetThreadAffinityMask(allLPs), which prevents Go-scheduler
	// migration but does not constrain LP placement. Useful when the
	// caller wants the deterministic-placement property of pinning
	// for reasons beyond coherency (e.g. cross-NUMA-node guarding on
	// hardware that reports a single L3 per socket).
	AffinityLocal AffinityMode = 2
)

// String returns a short label for diagnostics / logging.
func (m AffinityMode) String() string {
	switch m {
	case AffinityAuto:
		return "auto"
	case AffinityNone:
		return "none"
	case AffinityLocal:
		return "local"
	default:
		return "unknown"
	}
}

var (
	ccxMasksOnce sync.Once
	ccxMasks     []uint64
)

// CcxMasks returns the cached shared-L3 logical-processor masks for the
// current host. The first call probes Windows via
// GetLogicalProcessorInformationEx (see platform_windows.go); subsequent
// calls reuse the cached slice. Returns nil on systems where the topology
// query failed or no L3 was reported — callers should treat that as
// "affinity unavailable" and skip pinning.
func CcxMasks() []uint64 {
	ccxMasksOnce.Do(func() {
		ccxMasks = enumerateCcxMasks()
	})
	return ccxMasks
}

// affinityMaskForSlot computes the LP mask a worker for the given slot
// index should bind to under the given AffinityMode. Returns 0 when no
// pinning should occur:
//   - AffinityNone (explicit opt-out)
//   - empty CCX list (topology unavailable)
//   - AffinityAuto with len(masks) <= 1 (monolithic-L3 Intel desktop /
//     single-CCX Zen / constrained VM — no coherency benefit, so we skip
//     the LockOSThread + syscall to avoid the small runtime cost)
//
// AffinityLocal forces the pin even when len(masks) == 1; in that case
// the worker is bound to the single all-LP mask, which is a no-op for
// LP selection but still calls LockOSThread (deliberate — caller asked
// for explicit Local).
func affinityMaskForSlot(slotIdx int, mode AffinityMode) uint64 {
	if mode == AffinityNone {
		return 0
	}
	masks := CcxMasks()
	if len(masks) == 0 {
		return 0
	}
	if mode == AffinityAuto && len(masks) <= 1 {
		return 0
	}
	// AffinityAuto with multi-CCX, OR explicit AffinityLocal.
	return masks[slotIdx%len(masks)]
}

// pinSlotWorker is the canonical worker-side entry point invoked by every
// goroutine that owns a slot. It locks the goroutine to its current OS
// thread (so the affinity mask actually applies to the goroutine's
// execution) and sets the thread affinity to the slot's CCX mask. Caller
// must NOT runtime.UnlockOSThread until the goroutine exits — release would
// let the goroutine migrate off the pinned thread and silently lose the
// CCX locality. No-ops when mode is AffinityNone.
func pinSlotWorker(slotIdx int, mode AffinityMode) {
	mask := affinityMaskForSlot(slotIdx, mode)
	if mask == 0 {
		return
	}
	runtime.LockOSThread()
	pinCurrentThreadAffinity(mask)
}
