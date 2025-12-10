package shm

import (
	"os"
	"testing"
	"time"
	"unsafe"
)

func countFDs() int {
    entries, err := os.ReadDir("/proc/self/fd")
    if err != nil {
        return 0
    }
    return len(entries)
}

func TestLeakOnClose(t *testing.T) {
    shmName := "LeakTestSHM"
    // Cleanup first
    UnlinkShm(shmName)
    UnlinkEvent(shmName + "_slot_0")
    UnlinkEvent(shmName + "_slot_0_resp")

    // Create Host Resources
    totalSize := uint64(64 + 128 + 1024)
    if totalSize < 4096 { totalSize = 4096 }
    hShm, addr, err := CreateShm(shmName, totalSize)
    if err != nil {
        t.Fatalf("CreateShm failed: %v", err)
    }
    defer func() {
        CloseShm(hShm, addr, totalSize)
        UnlinkShm(shmName)
    }()

    // Init Header
    ex := (*ExchangeHeader)(unsafe.Pointer(addr))
    ex.Magic = Magic
    ex.Version = Version
    ex.NumSlots = 1
    ex.SlotSize = 1024
    ex.ReqOffset = 0
    ex.RespOffset = 512

    // Create Events
    hReq, _ := CreateEvent(shmName + "_slot_0")
    hResp, _ := CreateEvent(shmName + "_slot_0_resp")
    defer func() {
        CloseEvent(hReq)
        CloseEvent(hResp)
        UnlinkEvent(shmName + "_slot_0")
        UnlinkEvent(shmName + "_slot_0_resp")
    }()

    // Baseline
    initialFDs := countFDs()
    t.Logf("Initial FDs: %d", initialFDs)

    for i := 0; i < 50; i++ {
		guest, err := NewDirectGuest(shmName)
        if err != nil {
            t.Fatalf("Iteration %d: NewDirectGuest failed: %v", i, err)
        }

        // Start workers (to verify they exit)
        guest.Start(func(req, resp []byte, mt MsgType) (int32, MsgType) {
            return 0, MsgTypeNormal
        })

        // Close
        guest.Close()

        // Wait a bit
        time.Sleep(10 * time.Millisecond)
    }

    // Check FDs
    finalFDs := countFDs()
    t.Logf("Final FDs: %d", finalFDs)

    diff := finalFDs - initialFDs
    // Expect diff to be small (0 or 1-2 for fluctuation).
    // If it's > 40, it's definitely leaking.
    if diff > 40 {
        t.Fatalf("FD Leak detected! Increased by %d", diff)
    }
}
