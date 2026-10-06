//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Shared-memory clone without vfork or pthread semantics remains unsupported.
//
// XFAIL: *
// RUN: %ptsan_musl_strict -Wall -Wextra -Werror -pthread -o %t %S/clone.c
// RUN: %run %t clone-vm
