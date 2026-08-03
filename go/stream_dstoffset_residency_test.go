package shm

import (
	"reflect"
	"runtime"
	"testing"
	"unsafe"
)

// This file pins the RESIDENCY half of the SHM_VERSION 0x00080000 dstOffset
// change: an out-of-order chunk now costs ZERO parked payload copies.
//
// WHY IT IS NOT AN OUTPUT TEST. Reverse-order delivery produced byte-correct
// output under the OLD parking implementation too — it copied the payload into
// a side slice on arrival and copied it a second time into the destination when
// the gap filled. Asserting on delivered bytes therefore cannot distinguish the
// two implementations at all: it is the textbook silent-success trap. Neither can
// the parity table, and for a structural reason — see the header of
// go/stream_parity_table_test.go. What is measured here instead is how much memory
// the ahead-of-cursor bookkeeping makes resident: structurally (the parked record
// cannot even name payload bytes) and empirically (parked residency does not grow
// with chunk size).
//
// COMPANION: go/stream_dstoffset_placement_test.go pins the SEMANTICS of the same
// change — an ahead-of-cursor chunk's bytes are already at buf[dstOffset] while
// the stream is still incomplete. Together those two files are the entire guard
// for direct placement; nothing else in the suite can see it.

// TestStreamReassembly_ParkedRecordHoldsNoPayload is the structural half. It is
// a compile-time-ish property checked at runtime: streamContext.ooo's value type
// must be a fixed-size scalar record with no reference to payload bytes.
//
// The OLD implementation parked `map[uint32][]byte` — a value type of
// reflect.Slice, sized 24 bytes and pointing at a fresh copy of the chunk. Both
// assertions below reject that shape, which is precisely why they are here
// rather than an equivalent comment.
func TestStreamReassembly_ParkedRecordHoldsNoPayload(t *testing.T) {
	// Reached through the struct field rather than through placedChunk directly,
	// so swapping streamContext.ooo to a different value type is caught here too.
	// Via a nil pointer: streamContext carries a mutex, so a value copy would
	// trip `go vet -copylocks`.
	oooField, ok := reflect.TypeOf((*streamContext)(nil)).Elem().FieldByName("ooo")
	if !ok {
		t.Fatal("streamContext has no ooo field")
	}
	valueType := oooField.Type.Elem()

	if got := valueType.Size(); got > 16 {
		t.Errorf("parked record is %d bytes, want <= 16 — a parked entry must be O(1) bookkeeping, not a payload copy", got)
	}

	// Kind gate BEFORE NumField. The primary shape this test exists to reject is
	// the old `map[uint32][]byte`, whose value type is reflect.Slice — and
	// reflect.Type.NumField PANICS on any non-struct kind, so without this gate
	// the test would die with "reflect: NumField of non-struct type" and the
	// reader would never see either intended diagnostic (the size Errorf above is
	// not fatal, so it is reported, but the panic replaces the useful message
	// below with a reflect internal error).
	if k := valueType.Kind(); k != reflect.Struct {
		t.Fatalf("streamContext.ooo value type has kind %s (%s), want a struct — the out-of-order map must hold a payload-free bookkeeping RECORD; "+
			"a slice/pointer/interface value type means ahead-of-cursor chunks are being copied instead of written to buf[dstOffset] on arrival",
			k, valueType)
	}

	for i := 0; i < valueType.NumField(); i++ {
		f := valueType.Field(i)
		switch f.Type.Kind() {
		case reflect.Slice, reflect.Array, reflect.String, reflect.Ptr,
			reflect.UnsafePointer, reflect.Map, reflect.Interface,
			reflect.Chan, reflect.Func:
			t.Errorf("parked record field %s has kind %s — the out-of-order map must hold NO payload bytes and no reference to any; every chunk is written to buf[dstOffset] on arrival",
				f.Name, f.Type.Kind())
		}
	}

	// Sanity: the record must still carry enough to advance the cursor and
	// detect completion, or the drain could not validate anything.
	if _, ok := valueType.FieldByName("dstOffset"); !ok {
		t.Error("parked record has no dstOffset — the drain-time misplacement check has nothing to compare")
	}
	if _, ok := valueType.FieldByName("size"); !ok {
		t.Error("parked record has no size — the cursor cannot be advanced over a drained chunk")
	}
	if unsafe.Sizeof(placedChunk{}) != valueType.Size() {
		t.Fatalf("placedChunk (%d) is not the ooo value type (%d)", unsafe.Sizeof(placedChunk{}), valueType.Size())
	}
}

// TestStreamReassembly_ParkedResidencyIndependentOfChunkSize is the empirical
// half. The SAME permutation (strict reverse, so every chunk but the last parks)
// is delivered with 8-byte chunks and with 64 KiB chunks. The destination buffer
// is allocated by STREAM_START, before measurement begins, so what remains
// inside the window is exactly the ahead-of-cursor bookkeeping.
//
// Under the old parking implementation the window allocated (N-1) * chunkLen
// bytes of payload copies: ~504 B at 8-byte chunks and ~4.1 MB at 64 KiB — a
// 8000x spread. Under offset placement both are the same handful of map buckets.
// The assertion is on the DIFFERENCE between the two payload sizes, so it does
// not depend on Go's map growth constants.
func TestStreamReassembly_ParkedResidencyIndependentOfChunkSize(t *testing.T) {
	const totalChunks = 64
	const small = 8
	const large = 64 << 10

	grewSmall := measureReverseParkAlloc(t, small, totalChunks)
	grewLarge := measureReverseParkAlloc(t, large, totalChunks)

	// One chunk's worth of slack absorbs map-bucket growth and runtime noise
	// while still being 63x tighter than the old behavior at this chunk size.
	const slack = large
	if grewLarge > grewSmall+slack {
		t.Errorf("parked residency grew with payload size: %d bytes at %d-byte chunks vs %d bytes at %d-byte chunks (delta %d, want <= %d)\n"+
			"ahead-of-cursor chunks are being COPIED into the out-of-order map instead of written to buf[dstOffset] on arrival",
			grewLarge, large, grewSmall, small, grewLarge-grewSmall, slack)
	}
}

// measureReverseParkAlloc delivers totalChunks chunks of chunkLen bytes in
// strict reverse index order and returns the bytes the reassembler allocated
// while doing so, excluding the STREAM_START destination buffer. The minimum of
// several runs is taken so a background GC assist or goroutine allocation in one
// run cannot inflate the result.
func measureReverseParkAlloc(t *testing.T, chunkLen, totalChunks int) uint64 {
	t.Helper()

	const runs = 5
	best := ^uint64(0)
	for r := 0; r < runs; r++ {
		total := uint64(chunkLen * totalChunks)
		payload := make([]byte, chunkLen)
		for i := range payload {
			payload[i] = byte(i)
		}
		// Every request — header and payload — is built BEFORE the measurement
		// window, so the only allocations inside it belong to the reassembler.
		reqs := make([][]byte, totalChunks)
		for i := totalChunks - 1; i >= 0; i-- {
			reqs[i] = streamChunkReqAt(1, uint32(i), uint64(i)*uint64(chunkLen), payload)
		}

		var deliveredLen int
		var deliveries int
		handler := NewStreamReassembler(func(_ uint64, data []byte) {
			// Deliberately no copy: a copy here would allocate totalSize inside
			// the window and swamp the signal.
			deliveredLen = len(data)
			deliveries++
		}, nil)
		if _, mt := handler(streamStartReq(1, total, uint32(totalChunks)), nil, MsgTypeStreamStart); mt != MsgTypeNormal {
			t.Fatalf("StreamStart rejected: %v", mt)
		}

		runtime.GC()
		var before, after runtime.MemStats
		runtime.ReadMemStats(&before)
		for i := totalChunks - 1; i >= 0; i-- {
			if _, mt := handler(reqs[i], nil, MsgTypeStreamChunk); mt != MsgTypeNormal {
				t.Fatalf("chunk %d (%d-byte chunks) rejected: %v", i, chunkLen, mt)
			}
		}
		runtime.ReadMemStats(&after)

		if deliveries != 1 || deliveredLen != int(total) {
			t.Fatalf("delivered %d times with %d bytes, want 1 x %d — the measurement path is not actually reassembling", deliveries, deliveredLen, total)
		}
		if grew := after.TotalAlloc - before.TotalAlloc; grew < best {
			best = grew
		}
	}
	return best
}
