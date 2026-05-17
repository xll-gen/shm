#include "textflag.h"

// func cpuPause()
//
// Emits a single x86 PAUSE instruction. Used in the WaitStrategy spin loop
// to hint the CPU that this is a busy-wait, which (a) saves power, (b)
// avoids speculative-execution penalty on the eventual branch miss, and
// (c) under KVM/Hyper-V reduces cache-line ping-pong with the peer vCPU.
TEXT ·cpuPause(SB), NOSPLIT, $0-0
    PAUSE
    RET
