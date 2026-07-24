//go:build windows && !amd64

package shm

// xll-gen/shm targets **windows/amd64 exclusively**. This file exists only to
// FAIL THE BUILD on windows/386 (or any future non-amd64 Windows arch) with a
// clear message, instead of compiling silently and dying at runtime.
//
// Why a hard stop is required: the Win32 struct layouts in platform_windows.go
// — notably memoryBasicInformation (MEMORY_BASIC_INFORMATION) — are hand-laid
// for the amd64 ABI. Their field offsets assume an 8-byte uintptr (e.g.
// RegionSize at offset 24). On windows/386 uintptr is 4 bytes, so every field
// after AllocationBase shifts and VirtualQuery fills the wrong slots. The
// package still COMPILES (platform_windows.go carries no arch constraint, and
// spin_fallback.go once satisfied the non-amd64 spin primitives), but OpenShm's
// size check reads garbage and attach fails deterministically with a misleading
// "mapped view too small" error. A silent-build / runtime-death outcome is the
// worst case, so the arch is rejected here at compile time.
//
// See AGENTS.md "Platform Targets" and SPECIFICATION.md §4.4.
const _ = xllGenShm_requires_windows_amd64

// (referencing the undefined identifier above makes the compiler print its name
//  — that name IS the diagnostic. Do not define it.)
