/* SPDX-License-Identifier: GPL-3.0-only */
.text
.p2align 2
.global StartTrace
.type StartTrace, %function
StartTrace:
    // At the pinned OnDraw callsite x23 is the picture player. Pass it as
    // an additional argument without changing any of the caller's registers
    // other than the AAPCS64 scratch arguments. Original float stays in s0.
    mov x2, x30
    mov x3, x23
    b aurora_boot_trace_start
.size StartTrace, .-StartTrace

.global FinishTrace
.type FinishTrace, %function
FinishTrace:
    // Forward the original tag and the unmodified player's callsite.
    mov x1, x30
    mov x2, sp
    b aurora_boot_trace_finish
.size FinishTrace, .-FinishTrace
.section .note.GNU-stack,"",%progbits
