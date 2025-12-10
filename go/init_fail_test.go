package shm

import (
	"strings"
	"testing"
	"unsafe"
)

func TestInitFailOnEventError(t *testing.T) {
	// Create a name that is 250 chars long.
	// shm name: / + 250 = 251. Valid (NAME_MAX usually 255).
	// sem name: / + 250 + _slot_0 = 258. Invalid.
	// Length 250
	name := strings.Repeat("a", 250)

    UnlinkShm(name)
    defer UnlinkShm(name)

	// Create SHM
    // Use minimal size: Header + 1 Slot
    // Header 64
    // SlotHeader 128
    // SlotSize 64
    // Total 256
	hShm, addr, err := CreateShm(name, 4096)
	if err != nil {
		t.Fatalf("CreateShm failed: %v. Name length: %d", err, len(name))
	}
    defer CloseShm(hShm, addr, 4096)

    // Init Header
	exHeader := (*ExchangeHeader)(unsafe.Pointer(addr))
	exHeader.Magic = Magic
	exHeader.Version = Version
	exHeader.NumSlots = 1
	exHeader.NumGuestSlots = 0
	exHeader.SlotSize = 64
	exHeader.ReqOffset = 0
	exHeader.RespOffset = 32

    // Attempt NewDirectGuest
    // This should fail because creating/opening the event /aaaa..._slot_0 will fail (too long)
    _, err = NewDirectGuest(name, 0, 0)

    if err == nil {
        t.Errorf("Expected error from NewDirectGuest (invalid event name), got nil")
    } else {
        t.Logf("Got expected error: %v", err)
    }
}
