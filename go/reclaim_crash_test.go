package shm

import (
	"fmt"
	"os"
	"os/exec"
	"sync/atomic"
	"testing"
	"time"
	"unsafe"
)

// The v0.7.3 cross-process crash test re-execs the test binary as a
// short-lived "guest" child that claims a slot and exits without
// releasing it. The parent then exercises the reclamation API on the
// host-side shm to verify recovery across a real OS process boundary
// (kernel-tracked file/semaphore handles), not just the in-process CAS
// race that TestTryReclaim_NoDoubleClaim_Property covers.
//
// Mechanism: a TestMain-equivalent hook detects the env var
// SHM_CRASH_TEST_WORKER and dispatches to a worker entrypoint that
// re-opens the existing shm and exits abruptly. Without the env var the
// parent test runs normally.

const crashWorkerEnv = "SHM_CRASH_TEST_WORKER"

// crashTestSetup mirrors the boilerplate from TestSystemErrorReal:
// create shm, initialize ExchangeHeader, create per-slot events. Caller
// is responsible for the cleanup deferreds.
type crashTestSetup struct {
	name    string
	shmSize uint64
	h       ShmHandle
	addr    uintptr
	events  []EventHandle
}

func crashTestSetupName(t *testing.T) *crashTestSetup {
	t.Helper()
	s := &crashTestSetup{
		name:    fmt.Sprintf("shm_crash_v073_%d", time.Now().UnixNano()),
		shmSize: 4096,
	}
	var err error
	s.h, s.addr, err = createShm(s.name, s.shmSize)
	if err != nil {
		t.Fatalf("createShm: %v", err)
	}

	header := (*ExchangeHeader)(unsafe.Pointer(s.addr))
	header.Magic = Magic
	header.Version = Version
	header.NumSlots = 1
	header.NumGuestSlots = 1
	header.SlotSize = 1024
	header.ReqOffset = 0
	header.RespOffset = 512

	eventNames := []string{
		fmt.Sprintf("%s_slot_0", s.name),
		fmt.Sprintf("%s_slot_0_resp", s.name),
		fmt.Sprintf("%s_guest_call", s.name),
		fmt.Sprintf("%s_slot_1_resp", s.name),
	}
	for _, n := range eventNames {
		ev, err := createEvent(n)
		if err != nil {
			t.Fatalf("createEvent(%s): %v", n, err)
		}
		s.events = append(s.events, ev)
	}
	t.Cleanup(func() {
		for i, ev := range s.events {
			closeEvent(ev)
			unlinkEvent(eventNames[i])
		}
		closeShm(s.h, s.addr, s.shmSize)
		unlinkShm(s.name)
	})
	return s
}

// guestSlotHeader returns a pointer to the Guest slot's SlotHeader at
// the same address calc as TestSystemErrorReal (slot 1 = guest slot in
// a 1+1 layout).
func (s *crashTestSetup) guestSlotHeader() *SlotHeader {
	return (*SlotHeader)(unsafe.Pointer(s.addr + 1216))
}

// TestCrashProcess_ReclaimAfterChildExit re-execs this test binary as a
// child that claims the guest slot then immediately exits. The parent
// then verifies (a) the slot is in a non-FREE state with a non-zero
// lease, (b) TryReclaimAbandonedSlot succeeds once the lease ages past
// the threshold.
//
// The child does NOT use NewDirectGuest because that opens its own
// events and would race with the parent's setup. Instead it pokes the
// SlotHeader directly — the same fields a normal claim-CAS would
// modify (State, Lease) — modeling a guest that crashed precisely at
// the moment between claiming and releasing.
func TestCrashProcess_ReclaimAfterChildExit(t *testing.T) {
	if os.Getenv(crashWorkerEnv) != "" {
		crashWorkerMain()
		return
	}

	setup := crashTestSetupName(t)
	slotHdr := setup.guestSlotHeader()

	// Spawn child. Pass the shm name + base address... wait, the address
	// is per-process. The child has to re-open the shm itself and map it
	// in. Pass just the shm name via env; the child does its own mmap.
	exe, err := os.Executable()
	if err != nil {
		t.Fatalf("os.Executable: %v", err)
	}
	cmd := exec.Command(exe, "-test.run", "TestCrashProcess_ReclaimAfterChildExit", "-test.count=1")
	cmd.Env = append(os.Environ(),
		crashWorkerEnv+"=1",
		"SHM_CRASH_TEST_NAME="+setup.name,
	)
	cmd.Stdout = nil
	cmd.Stderr = nil
	if err := cmd.Start(); err != nil {
		t.Fatalf("child Start: %v", err)
	}
	// Child should exit quickly after claiming the slot. Bound the
	// wait so a hung child doesn't hang the test.
	done := make(chan error, 1)
	go func() { done <- cmd.Wait() }()
	select {
	case err := <-done:
		if err != nil {
			// Non-zero exit is fine — child intentionally simulates a
			// crash via os.Exit(0). Treat any other error path as test
			// infrastructure failure.
			if _, ok := err.(*exec.ExitError); !ok {
				t.Fatalf("child wait: %v", err)
			}
		}
	case <-time.After(10 * time.Second):
		_ = cmd.Process.Kill()
		t.Fatal("child did not exit within 10s")
	}

	// Verify the slot is in the post-claim state the child should have
	// left behind.
	state := atomic.LoadUint32(&slotHdr.State)
	lease := atomic.LoadUint64(&slotHdr.Lease)
	if state == SlotFree {
		t.Fatalf("child failed to claim slot — state is still SlotFree")
	}
	if lease == 0 {
		t.Fatalf("child failed to write lease — value is 0")
	}

	// Sleep past the reclaim threshold and verify recovery.
	const reclaimThreshold = 50 * time.Millisecond
	time.Sleep(reclaimThreshold + 50*time.Millisecond)

	// Construct a minimal DirectGuest pointing at our slot so we can
	// drive TryReclaimAbandonedSlot. We skip the slotContext.reqBuffer
	// wiring — TryReclaimAbandonedSlot only reads State/Lease.
	slots := []slotContext{{header: slotHdr}}
	g := &DirectGuest{slots: slots}

	if !g.TryReclaimAbandonedSlot(0, reclaimThreshold) {
		t.Fatalf("TryReclaimAbandonedSlot returned false; child-left-behind lease should be stale by now (state=%d lease=%d now=%d)",
			state, lease, MonotonicNanos())
	}
	if afterState := atomic.LoadUint32(&slotHdr.State); afterState != SlotFree {
		t.Fatalf("post-reclaim state = %d, want SlotFree", afterState)
	}
}

// crashWorkerMain is the child process entrypoint. Opens the existing
// shm by name, locates the guest slot, claims it (CAS Free→GuestBusy +
// write Lease), then exits without ever transitioning the slot back to
// Free. Mimics a guest process that crashed mid-handler.
func crashWorkerMain() {
	name := os.Getenv("SHM_CRASH_TEST_NAME")
	if name == "" {
		os.Exit(2)
	}
	// Re-open the existing shm. createShm in this codebase is creator-
	// or-opener depending on the OS, but the parent already created
	// the file/section; we expect the open to succeed.
	h, addr, err := createShm(name, 4096)
	if err != nil {
		os.Exit(3)
	}
	defer closeShm(h, addr, 4096)

	slotHdr := (*SlotHeader)(unsafe.Pointer(addr + 1216))
	// CAS Free → GuestBusy + write Lease. Skip on failure (parent will
	// observe SlotFree and fail its own assertion).
	if !atomic.CompareAndSwapUint32(&slotHdr.State, SlotFree, SlotGuestBusy) {
		os.Exit(4)
	}
	atomic.StoreUint64(&slotHdr.Lease, MonotonicNanos())
	// "Crash" — exit without releasing the slot.
	os.Exit(0)
}
