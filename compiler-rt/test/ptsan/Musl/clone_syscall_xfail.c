//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// The pointer-free fork form of SYS_clone is supported on AArch64, where
// musl uses it to implement fork. Other architectures still reject raw clone.
//
// XFAIL: x86_64-target-arch
// RUN: %ptsan_musl_strict -Wall -Wextra -Werror -pthread -o %t %S/clone.c
// RUN: %run %t syscall-clone
