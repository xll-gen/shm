#include "textflag.h"

// func spinUntilEq32(addr *uint32, want uint32, max uintptr) (iters uintptr, ok bool)
//
// Tight busy-wait: loads (*addr), compares against want, emits PAUSE between
// attempts. Returns the number of PAUSEd iterations consumed and whether the
// value was observed. Does NOT yield the Go scheduler — the caller must
// arrange runtime.Gosched() between bursts (see WaitStrategy.WaitState).
//
// Args layout (ABI0, all uintptr-equivalent slots):
//   addr  +0  (8)
//   want  +8  (4, padded to 8)
//   max   +16 (8)
//   iters +24 (8)
//   ok    +32 (1)
//
// Total args+returns = 33 bytes (size declared without trailing pad).
TEXT ·spinUntilEq32(SB), NOSPLIT, $0-33
    MOVQ    addr+0(FP), AX     // AX = &(*uint32)
    MOVL    want+8(FP), CX     // CX = want
    MOVQ    max+16(FP), DX     // DX = max iters
    XORQ    R8, R8             // R8 = iters consumed
loop:
    CMPQ    R8, DX
    JGE     fail
    MOVL    (AX), BX           // BX = atomic.LoadUint32(addr)
    CMPL    BX, CX
    JE      succ
    PAUSE
    INCQ    R8
    JMP     loop
succ:
    MOVQ    R8, iters+24(FP)
    MOVB    $1, ok+32(FP)
    RET
fail:
    MOVQ    R8, iters+24(FP)
    MOVB    $0, ok+32(FP)
    RET
