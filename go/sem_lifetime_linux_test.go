//go:build linux

package shm

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// TestSemaphoreLifetime_NoLeakAcrossRuns drives N init/teardown cycles of a
// host-style event creation and asserts that /dev/shm/sem.* does NOT grow
// across cycles — i.e. that UnlinkEvent fully reclaims POSIX named
// semaphores.
//
// Background: POSIX `sem_open(O_CREAT)` semantics keep the named semaphore
// alive in the kernel namespace (visible as /dev/shm/sem.*) until BOTH (a)
// every process has called sem_close on it, AND (b) someone has called
// sem_unlink. Forgetting (b) is silent on Linux: subsequent runs reopen the
// stale semaphore, inherit its leftover count, and produce spurious wakeups
// — a class of bug the host-side AGENTS.md backlog explicitly called out.
//
// This test is Linux-only (the contract is a no-op on Windows where named
// events are ref-counted by the kernel).
func TestSemaphoreLifetime_NoLeakAcrossRuns(t *testing.T) {
	const cycles = 50
	const namePrefix = "shm_semlt_test"

	baseline := countShmSemaphores(t, namePrefix)

	for i := 0; i < cycles; i++ {
		name := fmt.Sprintf("%s_%d", namePrefix, i)

		// Create — sem_open(O_CREAT) registers /dev/shm/sem.<name>.
		ev, err := CreateEvent(name)
		if err != nil {
			t.Fatalf("cycle %d: CreateEvent: %v", i, err)
		}
		// Close — releases this process's reference but does NOT remove
		// the inode (that's sem_unlink's job).
		CloseEvent(ev)
		// Unlink — removes the kernel inode. Without this call, every
		// cycle would add an entry to /dev/shm/sem.* and this test
		// would fail.
		UnlinkEvent(name)
	}

	final := countShmSemaphores(t, namePrefix)

	if final > baseline {
		// Best-effort cleanup so a partial leak doesn't poison later runs.
		t.Cleanup(func() {
			for i := 0; i < cycles; i++ {
				UnlinkEvent(fmt.Sprintf("%s_%d", namePrefix, i))
			}
		})
		t.Fatalf("semaphore leak: baseline=%d final=%d (delta=%d after %d cycles)",
			baseline, final, final-baseline, cycles)
	}
}

// countShmSemaphores scans /dev/shm for entries beginning with sem.<prefix>.
// Other test runs / unrelated processes are filtered out by the prefix so
// the count is specific to this test's namespace.
func countShmSemaphores(t *testing.T, prefix string) int {
	t.Helper()
	entries, err := os.ReadDir("/dev/shm")
	if err != nil {
		t.Fatalf("read /dev/shm: %v", err)
	}
	count := 0
	wanted := "sem." + prefix
	for _, e := range entries {
		if strings.HasPrefix(filepath.Base(e.Name()), wanted) {
			count++
		}
	}
	return count
}
