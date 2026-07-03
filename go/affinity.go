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
// case gracefully.
//
// AffinitySibling is the experimental opt-in that *explicitly* co-locates
// host and guest on the two SMT siblings of the same physical core so the
// SlotHeader state line ping-pong stays in shared L1d/L2 (~1-2 ns
// store-to-load forwarding) instead of bouncing through L3. See
// EXPERIMENTS.md §"2026-06-26 SMT-sibling co-location" for measured
// trade-offs. Falls back to no-pin when LTP_PC_SMT topology is absent
// (E-cores, no-SMT CPUs, constrained VMs).
type AffinityMode int

const (
	// AffinityAuto — default (zero value). Chipset-aware policy:
	//
	//  0. Oversubscription gate: if GOMAXPROCS < numSlots, do NOT pin. A
	//     pinned worker holds its P across each spin burst; with fewer Ps
	//     than workers, pinning adds P-contention without a locality payoff
	//     (runnable workers queue behind spinners for a P). Auto declines.
	//     This gate is Auto-only — explicit Local/Sibling are never gated.
	//  1. If the host reports SMT pairs (LTP_PC_SMT) AND numSlots fits
	//     within len(SmtPairs()), apply AffinitySibling-equivalent
	//     pinning (slot N → one SMT LP of physical core N % numPairs;
	//     C++ host pins to the other LP). This is the chiplet-AMD win
	//     measured 2026-06-26 (+24-74 % across the threads × payload
	//     matrix; see EXPERIMENTS.md §"2026-06-26 SMT-sibling
	//     co-location") and is expected (but unmeasured) to help on
	//     monolithic-L3 Intel SMT hosts as well.
	//  2. Otherwise, if the host reports >1 shared-L3 group, fall back
	//     to AffinityLocal-equivalent (CCX-wide mask). Covers the
	//     chiplet case when numSlots exceeds the SMT-pair count, plus
	//     multi-socket Xeon without exposed LTP_PC_SMT.
	//  3. Otherwise (monolithic L3, no SMT pairs, or constrained VM),
	//     no pinning — LockOSThread + same-as-all-LPs mask would add
	//     runtime overhead without coherency benefit.
	//
	// Versioning: v0.8.0 introduced opt-in pinning; v0.8.1 made Auto
	// apply CCX-wide pinning on multi-CCX hosts; v0.8.2 (this) extends
	// Auto to prefer SMT-sibling pinning whenever the topology supports
	// it. The change is monotonically better on the measured host and
	// degrades to v0.8.1 behaviour on hosts where Sibling is not safe
	// (numSlots > numPairs or LTP_PC_SMT unavailable).
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
	// AffinitySibling — opt-in, experimental. Each slot N pins its
	// guest goroutine to one specific SMT sibling LP of physical core
	// [N % numSmtCores]; the matching host worker thread (C++ side)
	// pins to the OTHER LP of the same physical core. Trades pipeline
	// resource sharing (ROB, LSQ, μop cache) for sub-ns cache-line
	// coherency on the SlotHeader state field. Empirical only — verify
	// in EXPERIMENTS.md / BENCHMARK_RESULTS.md before relying on it for
	// any production workload. Degenerates to no-pin if the host has
	// no SMT pairs reported by GetLogicalProcessorInformationEx
	// (RelationProcessorCore + LTP_PC_SMT).
	AffinitySibling AffinityMode = 3
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
	case AffinitySibling:
		return "sibling"
	default:
		return "unknown"
	}
}

// SmtPair describes one physical core's two SMT logical-processor masks.
// Host and Guest are single-bit KAFFINITY masks targeting the two LPs that
// share L1d/L2 on a single physical core. For non-SMT physical cores
// (single LP, or efficiency cores on hybrid CPUs) the entry is omitted
// from SmtPairs() — sibling-mode callers degrade to no-pin in that case.
type SmtPair struct {
	Host  uint64
	Guest uint64
}

var (
	ccxMasksOnce sync.Once
	ccxMasks     []uint64

	smtPairsOnce sync.Once
	smtPairs     []SmtPair
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

// SmtPairs returns the cached SMT-sibling LP-pair list for the current
// host. First call probes Windows via GetLogicalProcessorInformationEx
// (RelationProcessorCore + LTP_PC_SMT). Returns nil on hosts that report
// no SMT-capable physical cores; AffinitySibling falls back to no-pin in
// that case.
func SmtPairs() []SmtPair {
	smtPairsOnce.Do(func() {
		smtPairs = enumerateSmtPairs()
	})
	return smtPairs
}

// resolveAuto picks the concrete AffinityMode that AffinityAuto reduces
// to on the current host given numSlots. Exported semantics are
// documented on AffinityAuto. Non-Auto modes pass through unchanged.
//
// numSlots > 0 is required for the Sibling decision; callers that do
// not yet know it (e.g. ad-hoc one-shot pins) pass 0 to force the
// pre-v0.8.2 conservative behaviour (Local on multi-CCX, none elsewhere).
func resolveAuto(numSlots int, mode AffinityMode) AffinityMode {
	if mode != AffinityAuto {
		return mode
	}
	// Oversubscription gate. A pinned worker holds its P for each spin burst
	// (waitStrategyAsmChunk iterations between Gosched calls). When there are
	// fewer Ps than slot workers, pinning yields no cache-locality benefit and
	// instead deepens P-level contention — runnable workers wait behind
	// spinners for a P. Below the worker count, Auto declines to pin. Explicit
	// AffinityLocal/Sibling already returned above (caller owns that trade-off).
	if numSlots > 0 && runtime.GOMAXPROCS(0) < numSlots {
		return AffinityNone
	}
	pairs := SmtPairs()
	if numSlots > 0 && len(pairs) > 0 && numSlots <= len(pairs) {
		return AffinitySibling
	}
	if len(CcxMasks()) > 1 {
		return AffinityLocal
	}
	return AffinityNone
}

// affinityMaskForSlot computes the LP mask a worker for the given slot
// index should bind to under the given AffinityMode. Returns 0 when no
// pinning should occur:
//   - AffinityNone (explicit opt-out)
//   - AffinityAuto on a host where neither SMT pairs nor a multi-CCX
//     layout is available (monolithic-L3 Intel desktop / no-SMT VM)
//   - empty CCX / SMT-pair list (topology unavailable)
//
// AffinityLocal forces the pin even when len(masks) == 1; in that case
// the worker is bound to the single all-LP mask, which is a no-op for
// LP selection but still calls LockOSThread (deliberate — caller asked
// for explicit Local).
//
// AffinitySibling returns the guest LP of the [slotIdx % numPairs] SMT
// pair — host-side code (C++ benchmark binary or any C++ consumer that
// mirrors the policy) is expected to pin to the host LP of the same
// pair, putting the two endpoints on shared L1d/L2.
//
// numSlots is the total number of slot workers the caller is about to
// pin. Only AffinityAuto consults it (to decide whether Sibling is
// safe — see resolveAuto). For other modes it is ignored; pass 0 if
// unknown.
func affinityMaskForSlot(slotIdx, numSlots int, mode AffinityMode) uint64 {
	resolved := resolveAuto(numSlots, mode)
	switch resolved {
	case AffinityNone:
		return 0
	case AffinitySibling:
		pairs := SmtPairs()
		if len(pairs) == 0 {
			return 0
		}
		return pairs[slotIdx%len(pairs)].Guest
	case AffinityLocal:
		masks := CcxMasks()
		if len(masks) == 0 {
			return 0
		}
		return masks[slotIdx%len(masks)]
	default:
		return 0
	}
}

// pinSlotWorker is the canonical worker-side entry point invoked by every
// goroutine that owns a slot. It locks the goroutine to its current OS
// thread (so the affinity mask actually applies to the goroutine's
// execution) and sets the thread affinity to the slot's CCX mask. Caller
// must NOT runtime.UnlockOSThread until the goroutine exits — release would
// let the goroutine migrate off the pinned thread and silently lose the
// CCX locality. No-ops when the resolved mode yields a zero mask.
//
// numSlots is the total slot count the caller is about to pin; it feeds
// AffinityAuto's Sibling-vs-Local decision (see resolveAuto). For
// non-Auto modes it is ignored.
func pinSlotWorker(slotIdx, numSlots int, mode AffinityMode) {
	mask := affinityMaskForSlot(slotIdx, numSlots, mode)
	if mask == 0 {
		return
	}
	runtime.LockOSThread()
	pinCurrentThreadAffinity(mask)
}
