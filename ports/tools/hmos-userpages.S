/* SPDX-License-Identifier: GPL-2.0-only
 * Freestanding arm64 launcher for Aurora's experimental exec page-size ABI.
 * No libc or Debian userspace library is required. The kernel remains 16 KiB.
 * Usage: hmos-userpages /absolute/program [arguments...]
 */
#ifndef HMOS_PROCESS_PAGE_SIZE
#define HMOS_PROCESS_PAGE_SIZE 4096
#endif
.section .text
.global _start
.type _start, %function
_start:
    ldr x9, [sp]
#ifdef HMOS_BOOTSTRAP
    adr x20, bootstrap_argv
    add x21, sp, #16
    add x21, x21, x9, lsl #3
#else
    cmp x9, #2
    b.lt fail
    add x20, sp, #16             /* argv + 1 */
    add x21, sp, #16
    add x21, x21, x9, lsl #3     /* original envp */
#endif

    movz x0, #0x5003
    movk x0, #0x4155, lsl #16    /* PR_AURORA_SET_DEFAULT_PAGE_SIZE */
    mov x1, #HMOS_PROCESS_PAGE_SIZE
    mov x2, #0
    mov x3, #0
    mov x4, #0
    mov x8, #167                 /* __NR_prctl */
    svc #0
    cmp x0, #0
    b.lt fail

    movz x0, #0x5001
    movk x0, #0x4155, lsl #16    /* PR_AURORA_SET_EXEC_PAGE_SIZE */
    mov x1, #HMOS_PROCESS_PAGE_SIZE
    mov x2, #0
    mov x3, #0
    mov x4, #0
    mov x8, #167
    svc #0
    cmp x0, #0
    b.lt fail

    ldr x0, [x20]
    mov x1, x20
    mov x2, x21
    mov x8, #221                 /* __NR_execve */
    svc #0
fail:
    mov x0, #2
    adr x1, error_message
    mov x2, #(error_end-error_message)
    mov x8, #64                  /* __NR_write */
    svc #0
    mov x0, #125
    mov x8, #94                  /* __NR_exit_group */
    svc #0
    b fail
error_message:
    .ascii "hmos-userpages: page selection or exec failed (requires user4k=on)\n"
error_end:
.size _start, .-_start
#ifdef HMOS_BOOTSTRAP
.section .rodata
.balign 8
bootstrap_argv:
    .quad bootstrap_program, bootstrap_shell, bootstrap_script, 0
bootstrap_program:
    .asciz "/bin/busybox"
bootstrap_shell:
    .asciz "sh"
bootstrap_script:
    .asciz "/aurora-init"
#endif
.section .note.GNU-stack,"",%progbits
