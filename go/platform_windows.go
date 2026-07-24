package shm

import (
	"fmt"
	"syscall"
	"unsafe"
)

var (
	kernel32                              = syscall.NewLazyDLL("kernel32.dll")
	procCreateEventW                      = kernel32.NewProc("CreateEventW")
	procOpenEventW                        = kernel32.NewProc("OpenEventW")
	procSetEvent                          = kernel32.NewProc("SetEvent")
	procWaitForSingleObject               = kernel32.NewProc("WaitForSingleObject")
	procCloseHandle                       = kernel32.NewProc("CloseHandle")
	procCreateFileMappingW                = kernel32.NewProc("CreateFileMappingW")
	procOpenFileMappingW                  = kernel32.NewProc("OpenFileMappingW")
	procMapViewOfFile                     = kernel32.NewProc("MapViewOfFile")
	procUnmapViewOfFile                   = kernel32.NewProc("UnmapViewOfFile")
	procVirtualQuery                      = kernel32.NewProc("VirtualQuery")
	procGetLogicalProcessorInformationEx  = kernel32.NewProc("GetLogicalProcessorInformationEx")
	procSetThreadAffinityMask             = kernel32.NewProc("SetThreadAffinityMask")
	procGetCurrentThread                  = kernel32.NewProc("GetCurrentThread")
)

// memoryBasicInformation mirrors the Win32 MEMORY_BASIC_INFORMATION struct on
// amd64. Only RegionSize is read here; the explicit padding keeps RegionSize at
// its native offset (24) so VirtualQuery fills it correctly.
//
// amd64-ONLY LAYOUT (contract, not incidental): the field offsets below assume
// an 8-byte uintptr. On windows/386 (4-byte uintptr) RegionSize lands at the
// wrong offset and VirtualQuery mis-fills it. windows/amd64 is the sole
// supported target and windows/386 is rejected at compile time in
// platform_unsupported_arch.go — do NOT attempt to make this struct
// arch-portable.
type memoryBasicInformation struct {
	BaseAddress       uintptr // 0
	AllocationBase    uintptr // 8
	AllocationProtect uint32  // 16
	PartitionID       uint16  // 20
	_                 uint16  // 22 (pad to 8-align RegionSize)
	RegionSize        uintptr // 24
	State             uint32  // 32
	Protect           uint32  // 36
	Type              uint32  // 40
	_                 uint32  // 44 (tail pad to 48)
}

const (
	FILE_MAP_ALL_ACCESS = 0xF001F
	EVENT_ALL_ACCESS    = 0x1F0003
)

// createEvent implementation for Windows.
func createEvent(name string) (EventHandle, error) {
	n, err := syscall.UTF16PtrFromString("Local\\" + name)
	if err != nil {
		return 0, err
	}
	r1, _, err := procCreateEventW.Call(0, 0, 0, uintptr(unsafe.Pointer(n)))
	// CreateEvent returns NULL on failure
	if r1 == 0 {
		return 0, err
	}
	return EventHandle(r1), nil
}

// openEvent implementation for Windows.
func openEvent(name string) (EventHandle, error) {
	n, err := syscall.UTF16PtrFromString("Local\\" + name)
	if err != nil {
		return 0, err
	}
	// OpenEventW(dwDesiredAccess, bInheritHandle, lpName)
	r1, _, err := procOpenEventW.Call(uintptr(EVENT_ALL_ACCESS), 0, uintptr(unsafe.Pointer(n)))
	if r1 == 0 {
		return 0, err
	}
	return EventHandle(r1), nil
}

// signalEvent implementation for Windows.
//
// This goes through the runtime's standard stdcall path (g0 switch). A
// previous custom rawSyscall asm shim skipped that for speed, but it called
// kernel32 on the goroutine stack without the 16-byte RSP alignment the
// Microsoft x64 ABI requires (UB) and without stack headroom guarantees.
// SetEvent only fires when the peer is parked in an OS wait — already the
// slow path, where the kernel transition dwarfs the dispatch overhead.
func signalEvent(h EventHandle) {
	procSetEvent.Call(uintptr(h))
}

// waitForEvent implementation for Windows.
func waitForEvent(h EventHandle, timeoutMs uint32) {
	procWaitForSingleObject.Call(uintptr(h), uintptr(timeoutMs))
}

// closeEvent implementation for Windows.
func closeEvent(h EventHandle) {
	procCloseHandle.Call(uintptr(h))
}

// unlinkEvent implementation for Windows.
func unlinkEvent(name string) {
	// No-op on Windows (objects are ref-counted)
}

// createShm implementation for Windows.
func createShm(name string, size uint64) (ShmHandle, uintptr, error) {
	n, err := syscall.UTF16PtrFromString("Local\\" + name)
	if err != nil {
		return 0, 0, err
	}

	// CreateFileMappingW
	// High-order DWORD of size is size >> 32
	hMap, _, err := procCreateFileMappingW.Call(
		uintptr(syscall.InvalidHandle),
		0,
		uintptr(syscall.PAGE_READWRITE),
		uintptr(size>>32),
		uintptr(size&0xFFFFFFFF),
		uintptr(unsafe.Pointer(n)),
	)
	if hMap == 0 {
		return 0, 0, err
	}

	// MapViewOfFile
	addr, _, err := procMapViewOfFile.Call(
		hMap,
		uintptr(FILE_MAP_ALL_ACCESS),
		0,
		0,
		0,
	)
	if addr == 0 {
		procCloseHandle.Call(hMap)
		return 0, 0, err
	}

	return ShmHandle(hMap), addr, nil
}

// openShm implementation for Windows.
func openShm(name string, size uint64) (ShmHandle, uintptr, error) {
	n, err := syscall.UTF16PtrFromString("Local\\" + name)
	if err != nil {
		return 0, 0, err
	}

	// OpenFileMappingW(dwDesiredAccess, bInheritHandle, lpName)
	hMap, _, err := procOpenFileMappingW.Call(
		uintptr(FILE_MAP_ALL_ACCESS),
		0,
		uintptr(unsafe.Pointer(n)),
	)
	if hMap == 0 {
		return 0, 0, err
	}

	// MapViewOfFile
	addr, _, err := procMapViewOfFile.Call(
		hMap,
		uintptr(FILE_MAP_ALL_ACCESS),
		0,
		0,
		0,
	)
	if addr == 0 {
		procCloseHandle.Call(hMap)
		return 0, 0, err
	}

	// Validate the mapped view is at least `size` bytes before any caller
	// dereferences the ExchangeHeader/slots. MapViewOfFile with
	// dwNumberOfBytesToMap=0 maps the whole section, so a section created
	// smaller than expected (stale/mismatched config, or a Host still
	// initializing) would otherwise be read out of bounds. VirtualQuery reports
	// the committed region size at the view base; reject if it is short.
	var mbi memoryBasicInformation
	ret, _, qerr := procVirtualQuery.Call(addr, uintptr(unsafe.Pointer(&mbi)), unsafe.Sizeof(mbi))
	if ret == 0 {
		procUnmapViewOfFile.Call(addr)
		procCloseHandle.Call(hMap)
		return 0, 0, fmt.Errorf("VirtualQuery on mapped view failed: %v", qerr)
	}
	if uint64(mbi.RegionSize) < size {
		procUnmapViewOfFile.Call(addr)
		procCloseHandle.Call(hMap)
		return 0, 0, fmt.Errorf("mapped view too small: have %d bytes, need %d (host initializing or mismatched config?)", mbi.RegionSize, size)
	}

	return ShmHandle(hMap), addr, nil
}

// closeShm implementation for Windows.
func closeShm(h ShmHandle, addr uintptr, size uint64) {
	if addr != 0 {
		procUnmapViewOfFile.Call(addr)
	}
	if h != 0 {
		procCloseHandle.Call(uintptr(h))
	}
}

// unlinkShm implementation for Windows.
func unlinkShm(name string) {
	// No-op on Windows
}

// ---------------------------------------------------------------------------
// CPU affinity (opt-in; see AffinityMode in affinity.go).
// ---------------------------------------------------------------------------

const (
	// LOGICAL_PROCESSOR_RELATIONSHIP value for the RelationCache enumeration.
	relationCache = 2
	// LOGICAL_PROCESSOR_RELATIONSHIP value for RelationProcessorCore.
	relationProcessorCore = 0
	// PROCESSOR_CACHE_TYPE value for CacheUnified.
	cacheTypeUnified = 0
	// PROCESSOR_RELATIONSHIP.Flags bit indicating the physical core
	// hosts more than one logical processor (i.e. SMT enabled).
	ltpPcSmt = 0x1
)

// enumerateCcxMasks returns one KAFFINITY mask per shared-L3 logical-processor
// group reported by GetLogicalProcessorInformationEx(RelationCache). On
// chiplet CPUs (Ryzen / Threadripper / Epyc) each mask covers the LPs of a
// single CCX/CCD that share an L3 slice, so threads pinned to the same mask
// see ~30-50ns intra-CCX cache coherency instead of ~80-100ns cross-CCD via
// Infinity Fabric. Returns nil on systems that report no L3 (very old
// Windows, constrained VMs); the caller must treat that as "affinity not
// supported" and fall back to AffinityNone.
//
// SAFETY: the call returns the actual buffer length on the first probe and
// re-uses that length on the populated call; passing a too-small buffer
// receives ERROR_INSUFFICIENT_BUFFER (122) and is retried.
func enumerateCcxMasks() []uint64 {
	// Probe for the required buffer size.
	var need uint32
	procGetLogicalProcessorInformationEx.Call(uintptr(relationCache), 0, uintptr(unsafe.Pointer(&need)))
	if need == 0 {
		return nil
	}
	buf := make([]byte, need)
	r1, _, _ := procGetLogicalProcessorInformationEx.Call(
		uintptr(relationCache),
		uintptr(unsafe.Pointer(&buf[0])),
		uintptr(unsafe.Pointer(&need)),
	)
	if r1 == 0 {
		return nil
	}
	// SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX is variable-size, Size at
	// offset 4 gives the per-entry stride. For RelationCache the union
	// member is CACHE_RELATIONSHIP, and Windows 10 1709 grew its
	// Reserved field from DWORD Reserved[1] to BYTE Reserved[18] +
	// WORD GroupCount + (union GroupMask | GroupMasks[]) so the
	// GROUP_AFFINITY now starts at offset 32 within CACHE_RELATIONSHIP
	// (offset 40 from the outer struct base):
	//
	//   DWORD Relationship            // outer +0  (4)
	//   DWORD Size                    // outer +4  (4)
	//   CACHE_RELATIONSHIP {          // outer +8
	//     BYTE  Level                 // outer +8
	//     BYTE  Associativity         // outer +9
	//     WORD  LineSize              // outer +10
	//     DWORD CacheSize             // outer +12
	//     DWORD Type                  // outer +16 (PROCESSOR_CACHE_TYPE)
	//     BYTE  Reserved[18]          // outer +20
	//     WORD  GroupCount            // outer +38
	//     GROUP_AFFINITY GroupMask:   // outer +40
	//       KAFFINITY Mask            // outer +40 (8 on amd64)
	//       WORD Group                // outer +48
	//       WORD Reserved[3]          // outer +50
	//   }
	//
	// We require a single-group entry (GroupCount==1) — multi-group
	// (>64-LP partitions) is out of scope for the AffinityLocal trial.
	var masks []uint64
	off := uintptr(0)
	for off < uintptr(need) {
		rel := *(*uint32)(unsafe.Pointer(&buf[off]))
		size := *(*uint32)(unsafe.Pointer(&buf[off+4]))
		if rel == relationCache && size >= 56 { // need at least 1 GroupMask
			level := buf[off+8]
			cacheType := *(*uint32)(unsafe.Pointer(&buf[off+16]))
			groupCount := *(*uint16)(unsafe.Pointer(&buf[off+38]))
			if level == 3 && cacheType == cacheTypeUnified && groupCount == 1 {
				mask := *(*uint64)(unsafe.Pointer(&buf[off+40]))
				if mask != 0 {
					masks = append(masks, mask)
				}
			}
		}
		if size == 0 {
			// Defensive: stop on malformed entries rather than infinite-loop.
			break
		}
		off += uintptr(size)
	}
	return masks
}

// enumerateSmtPairs returns one (host, guest) single-LP mask pair per
// physical core reported by GetLogicalProcessorInformationEx
// (RelationProcessorCore) that has the LTP_PC_SMT flag set and exactly
// two LPs in its GroupMask. The lowest set bit becomes the host LP, the
// next-lowest becomes the guest LP — both sides (Go guest and C++ host)
// derive the same ordering so they end up on opposite SMT siblings of
// the same physical core.
//
// Skipped:
//   - PROCESSOR_RELATIONSHIP entries without LTP_PC_SMT (efficiency cores
//     on hybrid CPUs, no-SMT parts) — sibling mode requires shared L1d
//   - GroupCount != 1 (multi-group >64-LP partitions out of scope)
//   - cores reporting other than exactly 2 SMT siblings (SMT-4 POWER is
//     not a Windows/x86 case; defensive guard for unexpected topologies)
//
// Returns nil when no usable pair is found — caller (AffinitySibling)
// must treat that as "no pin" rather than fall back to a wider mask, to
// keep the experiment's invariant (host/guest on shared L1d) explicit.
func enumerateSmtPairs() []SmtPair {
	var need uint32
	procGetLogicalProcessorInformationEx.Call(uintptr(relationProcessorCore), 0, uintptr(unsafe.Pointer(&need)))
	if need == 0 {
		return nil
	}
	buf := make([]byte, need)
	r1, _, _ := procGetLogicalProcessorInformationEx.Call(
		uintptr(relationProcessorCore),
		uintptr(unsafe.Pointer(&buf[0])),
		uintptr(unsafe.Pointer(&need)),
	)
	if r1 == 0 {
		return nil
	}
	// SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX layout for RelationProcessorCore:
	//   DWORD Relationship            // outer +0  (4)  — must be 0
	//   DWORD Size                    // outer +4  (4)
	//   PROCESSOR_RELATIONSHIP {      // outer +8
	//     BYTE  Flags                 // outer +8       (LTP_PC_SMT = 0x1)
	//     BYTE  EfficiencyClass       // outer +9
	//     BYTE  Reserved[20]          // outer +10
	//     WORD  GroupCount            // outer +30
	//     GROUP_AFFINITY GroupMask[]: // outer +32
	//       KAFFINITY Mask            // outer +32 (8 on amd64)
	//       WORD Group                // outer +40
	//       WORD Reserved[3]          // outer +42
	//   }
	var pairs []SmtPair
	off := uintptr(0)
	for off < uintptr(need) {
		rel := *(*uint32)(unsafe.Pointer(&buf[off]))
		size := *(*uint32)(unsafe.Pointer(&buf[off+4]))
		if size == 0 {
			break
		}
		if rel == relationProcessorCore && size >= 48 {
			flags := buf[off+8]
			groupCount := *(*uint16)(unsafe.Pointer(&buf[off+30]))
			if flags&ltpPcSmt != 0 && groupCount == 1 {
				mask := *(*uint64)(unsafe.Pointer(&buf[off+32]))
				if popcount64(mask) == 2 {
					host := mask & (^mask + 1) // lowest set bit
					guest := mask & ^host       // remaining bit
					pairs = append(pairs, SmtPair{Host: host, Guest: guest})
				}
			}
		}
		off += uintptr(size)
	}
	return pairs
}

// popcount64 returns the Hamming weight of x. Plain Go to avoid a
// math/bits import bleeding into platform_windows.go's compile bag.
func popcount64(x uint64) int {
	n := 0
	for x != 0 {
		x &= x - 1
		n++
	}
	return n
}

// pinCurrentThreadAffinity binds the current OS thread to the given LP mask
// via SetThreadAffinityMask. Returns the previous mask (0 on failure). Caller
// must runtime.LockOSThread() first to keep the goroutine on this thread —
// otherwise the Go scheduler may migrate the goroutine to a different OS
// thread that does not carry the new affinity. Mask 0 is treated as "no
// change" rather than the Win32 "all LPs" interpretation, so we don't
// silently widen affinity on a degenerate input.
func pinCurrentThreadAffinity(mask uint64) uint64 {
	if mask == 0 {
		return 0
	}
	hThread, _, _ := procGetCurrentThread.Call()
	prev, _, _ := procSetThreadAffinityMask.Call(hThread, uintptr(mask))
	return uint64(prev)
}
