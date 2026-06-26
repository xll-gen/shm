#include "textflag.h"

// func spinUntilEq32(addr *uint32, want uint32, max uintptr) (iters uintptr, ok bool)
//
// Tight busy-wait: loads (*addr), compares against want, emits PAUSE between
// attempts. Returns the number of PAUSEd iterations consumed and whether the
// value was observed. Does NOT yield the Go scheduler — the caller must
// arrange runtime.Gosched() between bursts (see WaitStrategy.WaitState).
//
// PAUSE is issued every iteration (not skipped) — empirical sweep on a native
// Windows host (2026-06-26 harness.ps1) showed pause-every-N variants gave
// only marginal/inconsistent throughput gain while removing PAUSE regressed
// single-thread by up to 16% and burns SMT sibling bandwidth. PAUSE every
// iter is the right setting for HT cache-coherency hygiene.
//
// Loop entry is 16-byte aligned via PCALIGN so the back-edge target lands on
// a fresh instruction-fetch line. Loop body uses a single bottom-check
// (CMPQ/JL) instead of top+bottom, shaving one conditional branch per iter
// while preserving the zero-budget contract via an explicit max==0 guard at
// entry. Same-session A/B vs. the legacy top-check+no-align variant gave
// +2–4% at 1T/8T 64-byte ping-pong; the win is small but consistent and
// power-neutral.
//
// Args layout (ABI0, all uintptr-equivalent slots):
//   addr  +0  (8)
//   want  +8  (4, padded to 8)
//   max   +16 (8)
//   iters +24 (8)
//   ok    +32 (1)
TEXT ·spinUntilEq32(SB), NOSPLIT, $0-33
    MOVQ    addr+0(FP), AX     // AX = &(*uint32)
    MOVL    want+8(FP), CX     // CX = want
    MOVQ    max+16(FP), DX     // DX = max iters
    XORQ    R8, R8             // R8 = iters consumed
    TESTQ   DX, DX             // max==0: honour zero-budget contract → ok=false
    JZ      fail
    PCALIGN $16
loop:
    MOVL    (AX), BX           // BX = atomic.LoadUint32(addr)
    CMPL    BX, CX
    JE      succ
    PAUSE
    INCQ    R8
    CMPQ    R8, DX
    JL      loop
fail:
    MOVQ    R8, iters+24(FP)
    MOVB    $0, ok+32(FP)
    RET
succ:
    MOVQ    R8, iters+24(FP)
    MOVB    $1, ok+32(FP)
    RET
