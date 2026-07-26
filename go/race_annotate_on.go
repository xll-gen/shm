//go:build race

package shm

import (
	"runtime"
	"unsafe"
)

// Slot-ownership handoff annotations for the Go race detector (-race builds
// only; race_annotate_off.go compiles these to nothing).
//
// A guest slot is handed from one owner to the next through the atomics on
// SlotHeader.State, which lives in the file-mapped shared-memory region. Under
// the Go memory model those seq_cst atomics are a genuine release/acquire pair,
// so the handoff of the Go-heap-resident slotContext (nextMsgSeq, ActiveWait,
// waitStrategy) is correctly synchronized. The race detector, however, cannot
// see it: runtime.raceacquire/racerelease — and the compiler-inserted access
// instrumentation — drop every address outside [racearenastart, racearenaend)
// and [racedatastart, racedataend), and a MapViewOfFile region is in neither.
// So every ownership handoff through SlotHeader.State is invisible to tsan and
// the next owner's perfectly legal access to slotContext.nextMsgSeq is reported
// as a data race (e.g. StreamSender's pipelined chunk goroutines).
//
// These helpers re-publish the same edge on a Go-heap address — the
// slotContext itself — so the detector models what the hardware and the Go
// memory model already guarantee. They are annotations, not synchronization:
// production builds contain no trace of them.
//
// Placement rules, so genuine bugs stay visible:
//   - releaseSlotHandoff is called immediately BEFORE the state store/CAS that
//     publishes the slot as claimable, and ONLY by a goroutine that actually
//     owns it. A forfeited owner (GuestSlot.lost, or a timed-out
//     sendGuestCallInternal) publishes nothing and therefore annotates nothing —
//     so if it went on to touch the slot after a zombie steal, tsan would still
//     report it.
//   - acquireSlotHandoff is called immediately AFTER the claiming CAS succeeds.
func releaseSlotHandoff(s *slotContext) { runtime.RaceReleaseMerge(unsafe.Pointer(s)) }

func acquireSlotHandoff(s *slotContext) { runtime.RaceAcquire(unsafe.Pointer(s)) }
