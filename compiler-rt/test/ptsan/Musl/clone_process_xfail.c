//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Fork-like public clone remains unsupported until PTSan can repair the
// inherited runtime state in the child.
//
// XFAIL: *
// RUN: %ptsan_musl_strict -Wall -Wextra -Werror -pthread -o %t %S/clone.c
// RUN: %run %t clone-process
