//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Ptrace request values select incompatible scalar and pointer argument shapes.
//
// XFAIL: *
// RUN: %ptsan_musl_strict -Wall -Wextra -Werror -o %t %s
// RUN: %run %t

#include <stddef.h>
#include <sys/ptrace.h>

int main(void) { return (int)ptrace(PTRACE_TRACEME, 0, NULL, NULL); }
