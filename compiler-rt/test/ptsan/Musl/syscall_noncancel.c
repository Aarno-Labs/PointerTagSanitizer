//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// uname is a non-cancellation-point syscall in musl.  The good call must
// succeed, while the undersized output object must be rejected by PTSan before
// the kernel writes struct utsname.
//
// RUN: %ptsan_musl_strict -o %t %s
// RUN: %run %t good
// RUN: not %run %t bad 2> %t.err
// RUN: FileCheck %s < %t.err

// CHECK: Found bad

#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>

int main(int argc, char **argv) {
  if (argc != 2)
    return 1;

  if (strcmp(argv[1], "good") == 0) {
    struct utsname result;
    return uname(&result) == 0 ? 0 : 2;
  }

  if (strcmp(argv[1], "bad") != 0)
    return 3;

  char *buffer = malloc(sizeof(struct utsname) - 1);
  if (!buffer)
    return 4;
  (void)uname((struct utsname *)buffer);
  free(buffer);
  return 0;
}
