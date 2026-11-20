//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// System V semaphore control remains unsupported because each command gives
// its variadic argument a different scalar, structure, or array shape.
//
// XFAIL: *
// RUN: %ptsan_musl_strict -Wall -Wextra -Werror -o %t %s
// RUN: %run %t

#include <sys/sem.h>

int main(void) { return semctl(-1, 0, GETVAL); }
