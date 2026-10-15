//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict -O2 \
// RUN:   -mllvm -ptsan-abort-on-error -o %t %s
// RUN: not %t oob 2>&1 | FileCheck %s --check-prefix=OOB
// RUN: not %t use after-return 2>&1 | FileCheck %s --check-prefix=UAR
// OOB: checking repeatable alloca bounds
// OOB: Found bad
// UAR: checking retired repeatable alloca
// UAR: Found bad

#include <alloca.h>
#include <stdint.h>
#include <stdio.h>

static volatile unsigned size = 37;
static volatile unsigned count = 2;
static volatile uintptr_t escaped;

__attribute__((noinline)) static void write_byte(char *p, unsigned offset) {
  *(volatile char *)(p + offset) = 1;
}

// No fixed local buffer IDs: cleanup must run even when the only source IDs
// belong to the repeatable-alloca container.
__attribute__((noinline)) static void allocate(int oob) {
  unsigned n = size;
  for (unsigned i = 0; i < count; ++i) {
    char *p = alloca(n);
    write_byte(p, n - 1);
    escaped = (uintptr_t)p;
    if (oob) {
      fprintf(stderr, "checking repeatable alloca bounds\n");
      write_byte(p, n);
    }
  }
}

int main(int argc, char **argv) {
  allocate(argc == 2);
  fprintf(stderr, "checking retired repeatable alloca\n");
  write_byte((char *)escaped, 0);
  return 0;
}
