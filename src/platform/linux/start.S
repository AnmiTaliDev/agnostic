/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 AnmiTaliDev <anmitalidev@nuros.org> */
.text
.global _start
_start:
    xor %ebp, %ebp
    mov %rsp, %r12
    and $-16, %rsp
    mov (%r12), %rdi
    lea 8(%r12), %rsi
    mov %rdi, agn_argc(%rip)
    mov %rsi, agn_argv(%rip)
    call main
    mov %eax, %edi
    mov $231, %eax
    syscall
    hlt

.data
.global agn_argc
agn_argc: .quad 0
.global agn_argv
agn_argv: .quad 0

.section .note.GNU-stack,"",@progbits
