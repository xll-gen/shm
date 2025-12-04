#include "textflag.h"

// func rawSyscall(trap, a1, a2, a3 uintptr) (r1, r2, err uintptr)
TEXT ·rawSyscall(SB),NOSPLIT,$0-56
	MOVQ	trap+0(FP), AX
	MOVQ	a1+8(FP), CX
	MOVQ	a2+16(FP), DX
	MOVQ	a3+24(FP), R8

	SUBQ	$32, SP
	CALL	AX
	ADDQ	$32, SP

	MOVQ	AX, r1+32(FP)
	MOVQ	$0, r2+40(FP)
	MOVQ	$0, err+48(FP)
	RET
