//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Pointer-valued prctl outputs remain unsupported until PTSan can restore
// metadata to the address returned through the output cell.
//
// XFAIL: *
// RUN: %ptsan_musl_strict -Wall -Wextra -Werror -o %t %s
// RUN: %run %t

#include <sys/prctl.h>

int main(void) {
  int *clear_child_tid;
  return prctl(PR_GET_TID_ADDRESS, (unsigned long)&clear_child_tid, 0UL, 0UL,
               0UL);
}
