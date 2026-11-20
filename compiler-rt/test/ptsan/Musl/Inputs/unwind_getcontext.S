//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// This is the first pointer dereference performed by libunwind's x86-64
// __unw_getcontext implementation. Keeping it in a separate assembly file
// also preserves the real instrumented-code-to-assembly link boundary.

        .text
        .globl __unw_getcontext
        .type __unw_getcontext,@function
__unw_getcontext:
        movq %rax, (%rdi)
        xorl %eax, %eax
        retq
        .size __unw_getcontext, .-__unw_getcontext

        .weak unw_getcontext
        .set unw_getcontext, __unw_getcontext

        // The real implementation restores registers and transfers control.
        // A load and return are sufficient to test its raw assembly boundary.
        .globl __libunwind_Registers_x86_64_jumpto
        .type __libunwind_Registers_x86_64_jumpto,@function
__libunwind_Registers_x86_64_jumpto:
        movq (%rdi), %rax
        retq
        .size __libunwind_Registers_x86_64_jumpto, .-__libunwind_Registers_x86_64_jumpto

        .section .note.GNU-stack,"",@progbits
