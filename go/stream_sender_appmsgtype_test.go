package shm

import (
	"encoding/binary"
	"sync/atomic"
	"testing"
	"time"
)

// This file covers the WRITE side of StreamHeader.appMsgType@20 and NOTHING
// downstream of the wire. It proves the byte lands at offset 20 of the segment;
// it cannot tell whether anything ever reads it — for most of the field's life
// nothing did. The receive half lives in go/stream_apptype_delivery_test.go
// (positive: the tag reaches the handler) and in the
// AppMsgTypeIsInertToReassembly tripwire in go/stream_parity_table_test.go
// (negative: it changes nothing about reassembly). All three are needed.
//
// Every other appMsgType test hand-builds the StreamHeader bytes, so none of them
// touches StreamSender.SetAppMsgType or putStreamHeader. The tag could have been
// dropped, written to the wrong offset, or written to the chunk header instead,
// and the whole suite would still have been green. The tests below drive the real
// exported StreamSender against a real shared-memory segment and read the bytes
// back off the wire from the host side.

// appMsgTypeCapture is a mock host over a zombieStealEnv: it captures the request
// bytes of every guest-slot request it sees, tagged by MsgType, and ACKs.
type appMsgTypeCapture struct {
	env    *zombieStealEnv
	stop   chan struct{}
	done   chan struct{}
	frames chan capturedFrame
}

type capturedFrame struct {
	msgType MsgType
	bytes   []byte
}

func newAppMsgTypeCapture(env *zombieStealEnv) *appMsgTypeCapture {
	c := &appMsgTypeCapture{
		env:    env,
		stop:   make(chan struct{}),
		done:   make(chan struct{}),
		frames: make(chan capturedFrame, 64),
	}
	go c.run()
	return c
}

func (c *appMsgTypeCapture) run() {
	defer close(c.done)
	for {
		select {
		case <-c.stop:
			return
		default:
		}
		for k := 1; k <= c.env.numGuest; k++ {
			hdr := c.env.slotHeader(k)
			if atomic.LoadUint32(&hdr.State) != SlotReqReady {
				continue
			}
			if !atomic.CompareAndSwapUint32(&hdr.State, SlotReqReady, SlotBusy) {
				continue
			}
			// Snapshot exactly the bytes the sender declared, straight out of the
			// mapped request buffer — this is the wire, not a Go-side struct.
			n := int(hdr.ReqSize)
			buf := c.env.reqBuf(k)
			if n < 0 || n > len(buf) {
				n = 0
			}
			cp := make([]byte, n)
			copy(cp, buf[:n])
			select {
			case c.frames <- capturedFrame{msgType: hdr.MsgType, bytes: cp}:
			default:
			}

			hdr.RespSize = 0
			hdr.MsgType = MsgTypeNormal
			atomic.StoreUint32(&hdr.State, SlotRespReady)
			SignalEvent(c.env.respEvents[k])
		}
		time.Sleep(100 * time.Microsecond)
	}
}

func (c *appMsgTypeCapture) Close() {
	close(c.stop)
	<-c.done
}

// next returns the next captured frame of the requested type, failing if none
// arrives. Frames of other types are skipped (and reported, so a missing chunk is
// not silently swallowed).
func (c *appMsgTypeCapture) next(t *testing.T, want MsgType) capturedFrame {
	t.Helper()
	deadline := time.After(5 * time.Second)
	for {
		select {
		case f := <-c.frames:
			if f.msgType == want {
				return f
			}
			t.Logf("skipping captured frame of type %v (%d bytes)", f.msgType, len(f.bytes))
		case <-deadline:
			t.Fatalf("no %v frame reached the host within 5s", want)
		}
	}
}

// TestStreamSender_WritesAppMsgTypeAtWireOffset20 drives the REAL StreamSender
// (SetAppMsgType -> Send -> putStreamHeader -> shared memory) and reads
// StreamHeader.appMsgType back off the wire at byte offset 20.
//
// It also pins the rest of the STREAM_START header, because "the tag landed at 20"
// is only meaningful if it did not land there by displacing totalChunks.
func TestStreamSender_WritesAppMsgTypeAtWireOffset20(t *testing.T) {
	const appMsgType = uint32(0x1234ABCD)
	const streamID = uint64(0x0806)

	env := newZombieStealEnv(t, "StreamAppMsgTypeSHM", 2)
	defer env.Close()

	client, err := ConnectDefault(env.name)
	if err != nil {
		t.Fatalf("ConnectDefault failed: %v", err)
	}
	defer client.Close()

	host := newAppMsgTypeCapture(env)
	defer host.Close()

	sender := NewStreamSender(client, 1)
	sender.SetAppMsgType(appMsgType)

	data := make([]byte, 3000) // > slot payload, so several chunks follow
	for i := range data {
		data[i] = byte(i)
	}
	if err := sender.Send(data, streamID); err != nil {
		t.Fatalf("Send failed: %v", err)
	}

	start := host.next(t, MsgTypeStreamStart)
	if len(start.bytes) != streamHeaderSize {
		t.Fatalf("STREAM_START reqSize = %d, want %d (streamHeaderSize)", len(start.bytes), streamHeaderSize)
	}
	if got := binary.LittleEndian.Uint32(start.bytes[20:24]); got != appMsgType {
		t.Errorf("StreamHeader.appMsgType on the wire at offset 20 = 0x%08X, want 0x%08X\n"+
			"SetAppMsgType did not reach the wire. The write side is the ONLY half of appMsgType that works at "+
			"SHM_VERSION 0x00080000, so if this is wrong the field is entirely non-functional and no other test would notice "+
			"(every appMsgType reassembly test hand-builds the header).", got, appMsgType)
	}
	// The neighbouring fields must be undisturbed, or "offset 20" proves nothing.
	if got := binary.LittleEndian.Uint64(start.bytes[0:8]); got != streamID {
		t.Errorf("StreamHeader.streamID@0 = %d, want %d", got, streamID)
	}
	if got := binary.LittleEndian.Uint64(start.bytes[8:16]); got != uint64(len(data)) {
		t.Errorf("StreamHeader.totalSize@8 = %d, want %d", got, len(data))
	}
	if got := binary.LittleEndian.Uint32(start.bytes[16:20]); got == 0 || got == appMsgType {
		t.Errorf("StreamHeader.totalChunks@16 = %d — appMsgType appears to have displaced it", got)
	}

	// The tag belongs to STREAM_START only. ChunkHeader@20 is Padding and MUST
	// stay zero: a sender that wrote the routing tag there would be clobbering the
	// chunk header's reserved word on every chunk.
	chunk := host.next(t, MsgTypeStreamChunk)
	if len(chunk.bytes) < chunkHeaderSize {
		t.Fatalf("STREAM_CHUNK reqSize = %d, want >= %d", len(chunk.bytes), chunkHeaderSize)
	}
	if got := binary.LittleEndian.Uint32(chunk.bytes[20:24]); got != 0 {
		t.Errorf("ChunkHeader@20 (Padding) = 0x%08X, want 0 — appMsgType is a STREAM_START field only", got)
	}
	// And the chunk carries a real absolute destination, written by the real
	// putChunkHeader (chunk 0 of a fresh stream starts at 0).
	if got := binary.LittleEndian.Uint32(chunk.bytes[16:20]); got != 0 {
		t.Errorf("ChunkHeader.dstOffset@16 on chunk 0 = %d, want 0", got)
	}
}

// TestStreamSender_DefaultAppMsgTypeIsZero pins the unset default. Every sender
// that predates SHM_VERSION 0x00080000 wrote zero into that slot, so a
// StreamSender the caller never configured must be byte-identical to one.
func TestStreamSender_DefaultAppMsgTypeIsZero(t *testing.T) {
	env := newZombieStealEnv(t, "StreamAppMsgTypeZeroSHM", 1)
	defer env.Close()

	client, err := ConnectDefault(env.name)
	if err != nil {
		t.Fatalf("ConnectDefault failed: %v", err)
	}
	defer client.Close()

	host := newAppMsgTypeCapture(env)
	defer host.Close()

	sender := NewStreamSender(client, 1) // SetAppMsgType deliberately not called
	if err := sender.Send([]byte("hello"), 0x0807); err != nil {
		t.Fatalf("Send failed: %v", err)
	}

	start := host.next(t, MsgTypeStreamStart)
	if len(start.bytes) != streamHeaderSize {
		t.Fatalf("STREAM_START reqSize = %d, want %d", len(start.bytes), streamHeaderSize)
	}
	if got := binary.LittleEndian.Uint32(start.bytes[20:24]); got != 0 {
		t.Errorf("unconfigured StreamSender wrote appMsgType = 0x%08X, want 0 (the pre-0x00080000 value)", got)
	}
}
