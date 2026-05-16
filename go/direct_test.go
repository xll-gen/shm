package shm

import (
	"strings"
	"testing"
	"unsafe"
)

// TestNewDirectGuest_RejectsInvalidExchangeHeader exercises the relative-offset
// validation added to NewDirectGuest. A C++ host that corrupts the
// ExchangeHeader could publish (reqOffset, respOffset, slotSize) values that
// cause `respOffset - reqOffset` or `slotSize - respOffset` to underflow when
// the Go guest constructs zero-copy slices over the mapping. The validation
// rejects those cases up front instead of producing a runaway slice that
// escapes the mmap'd region.
func TestNewDirectGuest_RejectsInvalidExchangeHeader(t *testing.T) {
	cases := []struct {
		name        string
		reqOffset   uint32
		respOffset  uint32
		slotSize    uint32
		wantErrSubs string
	}{
		{
			name:        "RespBeforeReq",
			reqOffset:   32,
			respOffset:  16, // <= reqOffset → would underflow maxReq
			slotSize:    64,
			wantErrSubs: "respOffset (16) <= reqOffset (32)",
		},
		{
			name:        "RespEqualsReq",
			reqOffset:   32,
			respOffset:  32, // <= reqOffset (degenerate; zero-size request region)
			slotSize:    64,
			wantErrSubs: "respOffset (32) <= reqOffset (32)",
		},
		{
			name:        "RespBeyondSlot",
			reqOffset:   0,
			respOffset:  64, // >= slotSize → would underflow maxResp
			slotSize:    64,
			wantErrSubs: "respOffset (64) >= slotSize (64)",
		},
		{
			name:        "ReqBeyondSlot",
			reqOffset:   128,
			respOffset:  192, // both beyond slotSize; first check catches reqOffset
			slotSize:    64,
			wantErrSubs: "reqOffset (128) >= slotSize (64)",
		},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			// Unique name per case to avoid cross-test pollution.
			name := "invalid_exhdr_" + tc.name + "_" + strings.Repeat("x", 4)

			UnlinkShm(name)
			defer UnlinkShm(name)

			// Allocate enough room for the ExchangeHeader; NewDirectGuest only
			// reads the first 64 bytes for validation, so a small mapping is
			// sufficient — we never reach the per-slot mapping code.
			const mapSize = 4096
			h, addr, err := CreateShm(name, mapSize)
			if err != nil {
				t.Fatalf("CreateShm failed: %v", err)
			}
			defer CloseShm(h, addr, mapSize)

			exHeader := (*ExchangeHeader)(unsafe.Pointer(addr))
			exHeader.Magic = Magic
			exHeader.Version = Version
			exHeader.NumSlots = 1
			exHeader.NumGuestSlots = 0
			exHeader.SlotSize = tc.slotSize
			exHeader.ReqOffset = tc.reqOffset
			exHeader.RespOffset = tc.respOffset

			_, err = NewDirectGuest(name)
			if err == nil {
				t.Fatalf("expected error for case %q, got nil", tc.name)
			}
			if !strings.Contains(err.Error(), tc.wantErrSubs) {
				t.Fatalf("error %q does not contain expected substring %q", err.Error(), tc.wantErrSubs)
			}
		})
	}
}
