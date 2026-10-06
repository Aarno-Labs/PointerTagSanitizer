//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict -o %t %s
// RUN: %run %t
// RUN: not %run %t bad 2>&1 | FileCheck %s
// CHECK: Found bad

// getauxval traverses musl's saved auxv pointer. It must work after strict
// activation, preserve scalar values, and return a tagged AT_RANDOM pointer.
// Reading its 16 bytes is valid; reading the seventeenth must still report.
#include "../Inputs/ptsan_test_interface.h"
#include <elf.h>
#include <stdlib.h>
#include <sys/auxv.h>
#include <unistd.h>

__attribute__((constructor)) static void checkAuxiliaryVector(void) {
  if (getauxval(AT_PAGESZ) != (unsigned long)getpagesize() ||
      getauxval(AT_UID) != (unsigned long)getuid() ||
      getauxval(AT_PHENT) != sizeof(Elf64_Phdr))
    _Exit(2);

  volatile unsigned char *random = (void *)getauxval(AT_RANDOM);
  unsigned id = get_id(random);
  if (!id || id == ptsan_default_id() || ptsan_get_size(id) != 16)
    _Exit(3);
  for (int i = 0; i < 16; ++i)
    (void)random[i];
}

int main(int argc, char **argv) {
  checkAuxiliaryVector();
  if (argc > 1) {
    volatile unsigned char *random = (void *)getauxval(AT_RANDOM);
    (void)random[16];
  }
  // Avoid the unrelated strict-mode musl exit/TLS boundary.
  _Exit(0);
}
