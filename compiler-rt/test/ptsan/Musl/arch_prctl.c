//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Exercise the x86-only musl API that returns the hardware thread pointer.
// The kernel must receive an untagged output address, and PTSan must reject
// an output buffer too small for the returned unsigned long.
//
// REQUIRES: x86_64-target-arch
// RUN: %ptsan_musl_strict -o %t %s
// RUN: %run %t good
// RUN: not %run %t bad-arch-output 2> %t.output
// RUN: FileCheck %s < %t.output
//
// CHECK: Found bad

#include <stdlib.h>
#include <string.h>

// Musl exports arch_prctl without declaring it or the Linux operation values
// in its public headers.
extern int arch_prctl(int, unsigned long);
enum { ArchGetFs = 0x1003 };

static int test_good(void) {
  unsigned long fs_base = 0;
  if (arch_prctl(ArchGetFs, (unsigned long)&fs_base) != 0 || !fs_base) {
    return 1;
  }
  return 0;
}

static int test_bad_arch_output(void) {
  void *output = calloc(1, sizeof(unsigned long) - 1);
  if (!output) {
    return 1;
  }
  (void)arch_prctl(ArchGetFs, (unsigned long)output);
  free(output);
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    return 1;
  }
  if (!strcmp(argv[1], "good")) {
    return test_good();
  }
  if (!strcmp(argv[1], "bad-arch-output")) {
    return test_bad_arch_output();
  }
  return 2;
}
