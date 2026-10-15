//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Mount payloads are filesystem-specific and may contain nested pointers.
//
// XFAIL: *
// RUN: %ptsan_musl_strict -Wall -Wextra -Werror -o %t %s
// RUN: %run %t

#include <stddef.h>
#include <sys/mount.h>

int main(void) { return mount(NULL, NULL, NULL, 0, NULL); }
