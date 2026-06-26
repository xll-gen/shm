package shm

import (
	"runtime"
	"sync"
)

// AffinityMode selects how worker goroutines bind themselves to physical CPU
// resources. It is an opt-in client/host config field; the default zero
// value (AffinityNone) preserves backward-compatible OS-scheduler behaviour.
//
// On chiplet CPUs (Zen 2 / 3 / 4 / Threadripper / Epyc) the dominant
// 64-byte ping-pong cost between host and guest is the cross-CCD cache-line
// bounce (~80-100ns) traversing Infinity Fabric. Pinning the host worker
// thread and guest worker goroutine for the same slot index to the SAME
// CCX (shared L3) reduces that to ~30-50ns intra-CCX coherency, which can
// substantially improve multi-thread scaling efficiency in the benchmark
// matrix.
//
// SMT-sibling collision risk: AffinityLocal pins to a whole CCX mask (3+
// physical cores × 2 SMT siblings), so the OS scheduler chooses within the
// mask. PAUSE-every-iter in the spin loop handles the same-physical-core
// case gracefully. AffinityDedicated would pin to a single LP and risks the
// 2026-EXPERIMENTS.md §Exp 5 starvation pattern unless SMT-sibling exclusion
// is layered in — it is not implemented in this opt-in trial.
type AffinityMode int

const (
	// AffinityNone — no pinning; the OS scheduler decides placement.
	// Backward-compatible default.
	AffinityNone AffinityMode = 0
	// AffinityLocal — pin each worker goroutine to the CCX (shared-L3
	// group) at index `slotIdx % len(EnumerateCcxMasks())`. Two slots N
	// and N+numCCX land on the same CCX. A host and guest that follow
	// the same deterministic mapping co-locate on the same L3 region.
	AffinityLocal AffinityMode = 1
)

// String returns a short label for diagnostics / logging.
func (m AffinityMode) String() string {
	switch m {
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
// pinning should occur (AffinityNone, empty CCX list, or any future
// degenerate mode).
func affinityMaskForSlot(slotIdx int, mode AffinityMode) uint64 {
	if mode == AffinityNone {
		return 0
	}
	masks := CcxMasks()
	if len(masks) == 0 {
		return 0
	}
	if mode == AffinityLocal {
		return masks[slotIdx%len(masks)]
	}
	return 0
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
