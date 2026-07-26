package shm

import (
	"fmt"
	"testing"
	"unsafe"
)

// maxReqTestMapping stands up a real shared-memory segment with the geometry a
// C++ DirectHost::Init would publish (reqOffset=0, respOffset=slotSize/2) plus
// every named event NewDirectGuest opens, then connects a Client to it.
// The returned cleanup closes the client and unlinks everything.
func maxReqTestMapping(t *testing.T, shmName string, slotSize uint32, numSlots, numGuestSlots uint32) (*Client, func()) {
	t.Helper()

	total := numSlots + numGuestSlots
	names := []string{fmt.Sprintf("%s_guest_call", shmName)}
	for i := uint32(0); i < numSlots; i++ {
		names = append(names, fmt.Sprintf("%s_slot_%d", shmName, i))
	}
	for i := uint32(0); i < total; i++ {
		names = append(names, fmt.Sprintf("%s_slot_%d_resp", shmName, i))
	}

	UnlinkShm(shmName)
	for _, n := range names {
		UnlinkEvent(n)
	}

	totalSize := uint64(64) + uint64(total)*(128+uint64(slotSize))
	if totalSize < 4096 {
		totalSize = 4096
	}

	hShm, addr, err := CreateShm(shmName, totalSize)
	if err != nil {
		t.Fatalf("CreateShm: %v", err)
	}

	ex := (*ExchangeHeader)(unsafe.Pointer(addr))
	ex.Magic = Magic
	ex.Version = Version
	ex.NumSlots = numSlots
	ex.NumGuestSlots = numGuestSlots
	ex.SlotSize = slotSize
	ex.ReqOffset = 0
	ex.RespOffset = slotSize / 2

	handles := make([]EventHandle, 0, len(names))
	for _, n := range names {
		h, err := CreateEvent(n)
		if err != nil {
			t.Fatalf("CreateEvent(%s): %v", n, err)
		}
		handles = append(handles, h)
	}

	c, err := ConnectDefault(shmName)
	if err != nil {
		t.Fatalf("ConnectDefault: %v", err)
	}

	cleanup := func() {
		c.Close()
		for i, h := range handles {
			CloseEvent(h)
			UnlinkEvent(names[i])
		}
		CloseShm(hShm, addr, totalSize)
		UnlinkShm(shmName)
	}
	return c, cleanup
}

// TestMaxRequestSize_MatchesSlotBuffer asserts the accessor reports exactly
// what a claimed slot's RequestBuffer() exposes — the property that lets a
// consumer drop the claim-and-Release probe — and that it tracks the Host's
// published geometry rather than a hardcoded constant.
func TestMaxRequestSize_MatchesSlotBuffer(t *testing.T) {
	cases := []struct {
		name     string
		slotSize uint32
	}{
		{"1KiB", 1024},
		{"256KiB", 256 * 1024},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			shmName := "MaxReqSizeSHM_" + tc.name
			c, cleanup := maxReqTestMapping(t, shmName, tc.slotSize, 1, 2)
			defer cleanup()

			want := int(tc.slotSize / 2)
			if got := c.MaxRequestSize(); got != want {
				t.Fatalf("Client.MaxRequestSize() = %d, want %d", got, want)
			}
			if got := c.guest.MaxRequestSize(); got != want {
				t.Fatalf("DirectGuest.MaxRequestSize() = %d, want %d", got, want)
			}

			// Guest slot: the exposure the accessor replaces.
			slot, err := c.AcquireGuestSlot()
			if err != nil {
				t.Fatalf("AcquireGuestSlot: %v", err)
			}
			if got := len(slot.RequestBuffer()); got != c.MaxRequestSize() {
				t.Fatalf("len(RequestBuffer()) = %d, MaxRequestSize() = %d", got, c.MaxRequestSize())
			}
			if got := len(slot.slot.reqBuffer); got != want {
				t.Fatalf("len(slot.reqBuffer) = %d, want %d", got, want)
			}
			slot.Release()

			// Host slots share the one published geometry: same value.
			for i := range c.guest.slots {
				if got := len(c.guest.slots[i].reqBuffer); got != want {
					t.Fatalf("slot %d reqBuffer = %d, want %d (host/guest geometry must match)", i, got, want)
				}
			}
		})
	}
}

// TestMaxRequestSize_NilAndUnconnected pins the documented degenerate-receiver
// contract: 0, never a panic. Consumers are expected to call this on a client
// handle that may be a typed nil while the transport is still coming up.
func TestMaxRequestSize_NilAndUnconnected(t *testing.T) {
	var nilClient *Client
	if got := nilClient.MaxRequestSize(); got != 0 {
		t.Fatalf("(*Client)(nil).MaxRequestSize() = %d, want 0", got)
	}

	if got := (&Client{}).MaxRequestSize(); got != 0 {
		t.Fatalf("unconnected Client.MaxRequestSize() = %d, want 0", got)
	}

	var nilGuest *DirectGuest
	if got := nilGuest.MaxRequestSize(); got != 0 {
		t.Fatalf("(*DirectGuest)(nil).MaxRequestSize() = %d, want 0", got)
	}
}
