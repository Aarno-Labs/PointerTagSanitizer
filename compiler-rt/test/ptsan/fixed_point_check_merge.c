//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -fno-vectorize -fno-slp-vectorize \
// RUN:   -fno-unroll-loops -S -emit-llvm -o %exe.ll %s
// RUN: FileCheck %s --check-prefix=IR < %exe.ll
// RUN: %clang_ptsan -g -O2 -fno-vectorize -fno-slp-vectorize \
// RUN:   -fno-unroll-loops -o %exe %s
// RUN: ./%exe 3 2> good.txt
// RUN: FileCheck %s --check-prefix=GOOD < good.txt
// RUN: ./%exe 2 2> bad.txt
// RUN: FileCheck %s --check-prefix=BAD < bad.txt

// The first merging pass combines the checks for offsets 0 and 2. A second
// pass is then needed to notice that their combined range subsumes offset 1.

// IR-LABEL: define internal fastcc void @write_out_of_order
// IR: call void @ptsan_complain
// IR-NOT: call void @ptsan_complain
// IR: ret void

// GOOD-NOT: Found bad
// BAD: Found bad COMPOUND access

#include <stdint.h>
#include <stdlib.h>

__attribute__((noinline)) static void
write_out_of_order(volatile uint8_t *Buffer) {
  Buffer[0] = 1;
  Buffer[2] = 2;
  Buffer[1] = 3;
}

int main(int Argc, char **Argv) {
  size_t Size = Argc > 1 ? (size_t)atoi(Argv[1]) : 3;
  uint8_t *Buffer = (uint8_t *)malloc(Size);
  if (!Buffer) {
    return 1;
  }

  write_out_of_order(Buffer);
  free(Buffer);
  return 0;
}
