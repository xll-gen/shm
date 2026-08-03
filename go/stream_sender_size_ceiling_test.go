package shm

import (
	"encoding/binary"
	"strconv"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

// The SENDER half of the 1 GiB wire ceiling (SPEC §3.3.2 clause 2, §3.3.4 bounds
// table). Go had only the RECEIVER's bound: MaxStreamSize was consulted in
// go/stream.go's STREAM_START path and nowhere else, while StreamSender.sendStart
// wrote uint64(len(data)) unchecked and sendChunk narrowed the absolute offset
// with uint32(dstOffset).
//
// Why relying on the peer is not equivalent. Past 4 GiB the uint32 narrowing
// wraps, so a chunk lands at a wrong but IN-BOUNDS address — a legal-looking value
// the receiver's bounds check cannot reject. Between 1 and 4 GiB a conforming C++
// peer does refuse the STREAM_START, which is why the runtime exposure today is
// small, but SPEC states the sender's duty as a MUST precisely so that correctness
// does not rest on the peer's diligence: "It MUST NOT rely on the receiver's
// STREAM_START bound to catch the case for it."
//
// This is the Go twin of tests/test_stream_size_ceiling.cpp's senderHalf(), and
// like it no reassembler is involved. Where the C++ twin can cheaply try 2 GiB,
// 4 GiB+1 and SIZE_MAX — its API takes a `size` independent of the buffer, so it
// passes `&oneByte` — Go's takes a slice, so the declared size IS the allocation.
// The guard is one `len(data) > MaxStreamSize` comparison, so a single
// just-over-the-line case exercises the same branch; the extra magnitudes only
// change the number printed. An earlier draft fabricated the length with
// unsafe.Slice over a 64-byte array, which is cheap AND covers 4 GiB — but `-race`
// enables checkptr and fatals with "unsafe.Slice result straddles multiple
// allocations", and a test that only runs without -race is worse than a slower one.

// hugeZeroSlice allocates n real bytes. Nothing touches them — every test below
// stops the sender before the chunk phase — so Windows never faults the pages in:
// this costs ~2 ms and ~340 ms under -race, and no resident memory.
func hugeZeroSlice(n int) []byte { return make([]byte, n) }

// ceilingRejectHost is a mock host over a zombieStealEnv that captures every
// guest-slot request it sees and answers MsgTypeSystemError.
//
// Answering SYSTEM_ERROR rather than ACKing is load-bearing for BOTH tests below:
// sendStart surfaces it as an error and Send returns without entering the chunk
// phase, so no 1 GiB is ever copied and no goroutine outlives the test still
// spinning on the slot header that the deferred env.Close is about to unmap. It
// also makes the observation deterministic — the question is whether a
// STREAM_START reached the host, not how long Send took.
type ceilingRejectHost struct {
	env    *zombieStealEnv
	stop   chan struct{}
	done   chan struct{}
	seen   int32
	frames chan capturedFrame
}

func newCeilingRejectHost(env *zombieStealEnv) *ceilingRejectHost {
	h := &ceilingRejectHost{
		env:    env,
		stop:   make(chan struct{}),
		done:   make(chan struct{}),
		frames: make(chan capturedFrame, 16),
	}
	go h.run()
	return h
}

func (h *ceilingRejectHost) run() {
	defer close(h.done)
	for {
		select {
		case <-h.stop:
			return
		default:
		}
		for k := 1; k <= h.env.numGuest; k++ {
			hdr := h.env.slotHeader(k)
			if atomic.LoadUint32(&hdr.State) != SlotReqReady {
				continue
			}
			if !atomic.CompareAndSwapUint32(&hdr.State, SlotReqReady, SlotBusy) {
				continue
			}
			n := int(hdr.ReqSize)
			buf := h.env.reqBuf(k)
			if n < 0 || n > len(buf) {
				n = 0
			}
			cp := make([]byte, n)
			copy(cp, buf[:n])
			select {
			case h.frames <- capturedFrame{msgType: hdr.MsgType, bytes: cp}:
			default:
			}
			atomic.AddInt32(&h.seen, 1)

			hdr.RespSize = 0
			hdr.MsgType = MsgTypeSystemError
			atomic.StoreUint32(&hdr.State, SlotRespReady)
			SignalEvent(h.env.respEvents[k])
		}
		time.Sleep(100 * time.Microsecond)
	}
}

func (h *ceilingRejectHost) Close() {
	close(h.stop)
	<-h.done
}

func (h *ceilingRejectHost) count() int { return int(atomic.LoadInt32(&h.seen)) }

// TestStreamSender_RefusesStreamAboveWireCeiling asserts that an over-ceiling
// stream is refused BEFORE anything is transmitted: the error names the size and
// the bound, and not one byte reached the host — which is the observable form of
// "before transmitting anything, and not by relying on the receiver's bound".
func TestStreamSender_RefusesStreamAboveWireCeiling(t *testing.T) {
	env := newZombieStealEnv(t, "StreamSizeCeilingSHM", 1)
	defer env.Close()

	client, err := ConnectDefault(env.name)
	if err != nil {
		t.Fatalf("ConnectDefault failed: %v", err)
	}
	defer client.Close()

	host := newCeilingRejectHost(env)
	defer host.Close()

	const size = MaxStreamSize + 1
	sender := NewStreamSender(client, 1)

	err = sender.Send(hugeZeroSlice(size), 0x0808)
	if err == nil {
		t.Fatalf("Send accepted a stream of %d bytes; ChunkHeader.dstOffset cannot express its offsets, and past 4 GiB the uint32 narrowing wraps into a legal-looking wrong address", size)
	}
	msg := err.Error()
	if !strings.Contains(msg, strconv.Itoa(size)) || !strings.Contains(msg, strconv.Itoa(MaxStreamSize)) {
		t.Errorf("Send failed with %q, which does not name both the offending size (%d) and the bound (%d).\n"+
			"If this reads like a host rejection, the sender is relying on the peer's STREAM_START bound, which SPEC 3.3.2 clause 2 forbids in so many words.",
			msg, size, MaxStreamSize)
	}

	// THE ORDERING ASSERTION. SPEC 3.3.2 clause 2: "A sender MUST refuse, BEFORE
	// TRANSMITTING ANYTHING". The mock host answers every request it sees, so a
	// sender that got as far as publishing a STREAM_START is visible here even
	// though Send still ends up returning an error.
	if n := host.count(); n != 0 {
		t.Errorf("%d request(s) reached the host for a stream that must be refused before transmitting anything; a guest slot was consumed and a STREAM_START published", n)
	}
	hdr := env.slotHeader(1)
	if st := atomic.LoadUint32(&hdr.State); st != SlotFree {
		t.Errorf("guest slot 1 state = %d, want SlotFree (%d)", st, SlotFree)
	}
	if hdr.ReqSize != 0 {
		t.Errorf("guest slot 1 ReqSize = %d, want 0 — a refused stream published a request", hdr.ReqSize)
	}
}

// TestStreamSender_AcceptsStreamAtWireCeilingBoundary pins the comparison as
// strict (>), so the largest stream the wire format can express stays sendable.
// The at-ceiling STREAM_START must actually reach the host carrying
// totalSize == MaxStreamSize; the host then refuses it, which is what keeps the
// test off the 1 GiB chunk phase.
func TestStreamSender_AcceptsStreamAtWireCeilingBoundary(t *testing.T) {
	env := newZombieStealEnv(t, "StreamSizeCeilingEdgeSHM", 1)
	defer env.Close()

	client, err := ConnectDefault(env.name)
	if err != nil {
		t.Fatalf("ConnectDefault failed: %v", err)
	}
	defer client.Close()

	host := newCeilingRejectHost(env)
	defer host.Close()

	sender := NewStreamSender(client, 1)
	err = sender.Send(hugeZeroSlice(MaxStreamSize), 0x0809)
	if err == nil {
		t.Fatalf("Send returned success although the mock host refuses every request; the test is not exercising what it thinks")
	}
	if strings.Contains(err.Error(), "wire ceiling") {
		t.Fatalf("Send refused a stream of exactly MaxStreamSize (%d) with %q.\n"+
			"The ceiling is inclusive: the guard must be `> MaxStreamSize`, not `>=`, or the largest stream the wire format can express becomes unsendable.",
			MaxStreamSize, err)
	}

	// Positive proof it was transmitted rather than refused locally, and that the
	// advertised size is the ceiling itself.
	select {
	case f := <-host.frames:
		if f.msgType != MsgTypeStreamStart {
			t.Fatalf("first frame reaching the host was %v, want %v", f.msgType, MsgTypeStreamStart)
		}
		if len(f.bytes) != streamHeaderSize {
			t.Fatalf("STREAM_START reqSize = %d, want %d", len(f.bytes), streamHeaderSize)
		}
		if got := binary.LittleEndian.Uint64(f.bytes[8:16]); got != MaxStreamSize {
			t.Errorf("StreamHeader.totalSize@8 = %d, want %d (MaxStreamSize)", got, MaxStreamSize)
		}
	case <-time.After(5 * time.Second):
		t.Fatalf("no STREAM_START reached the host for a stream of exactly MaxStreamSize (%d) — the at-ceiling stream was refused locally instead of being sent", MaxStreamSize)
	}
}
