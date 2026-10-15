//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: split-file %s %t.split
// RUN: %clang_ptsan -O2 -mllvm -ptsan-abort-on-error -I%S -o %t \
// RUN:   %t.split/main.c %t.split/read.c
// RUN: %run %t 2>&1 | FileCheck %s --check-prefix=GOOD
// RUN: not %run %t bad 2>&1 | FileCheck %s --check-prefix=BAD
// RUN: %clang_ptsan -O2 -fno-pie -no-pie -mllvm -ptsan-abort-on-error \
// RUN:   -I%S -o %t.nopie %t.split/main.c %t.split/read.c
// RUN: %run %t.nopie 2>&1 | FileCheck %s --check-prefix=GOOD
// RUN: not %run %t.nopie bad 2>&1 | FileCheck %s --check-prefix=BAD
// RUN: %clang_ptsan -O2 -fPIE -pie -mllvm -ptsan-abort-on-error \
// RUN:   -I%S -o %t.pie %t.split/main.c %t.split/read.c
// RUN: %run %t.pie 2>&1 | FileCheck %s --check-prefix=GOOD
// RUN: not %run %t.pie bad 2>&1 | FileCheck %s --check-prefix=BAD

// GOOD-NOT: Found bad
// GOOD: shared-constant-access-ok
// GOOD-NOT: Found bad
// BAD: Found bad POINT access
// BAD-NOT: outside-read-completed

//--- main.c
#include "Inputs/ptsan_test_interface.h"

#include <assert.h>

static const unsigned char First[] = "first constant";
static const unsigned char Second[] = "second constant";
static unsigned char Writable = 0x5a;

unsigned char read_constant_offset(const unsigned char *base, intptr_t offset);

static intptr_t raw_address(const void *p) {
  return (uintptr_t)p & ((UINT64_C(1) << 48) - 1);
}

int main(int argc, char **argv) {
  (void)argv;
  unsigned id = get_id(First);
  assert(id != 0 && id != ptsan_default_id());
  assert(id == get_id(Second));
  assert(raw_address(First) != raw_address(Second));

  // Deliberately leave the source object while staying in its shared segment.
  // Integer offsets avoid assuming an ordering or subtracting unrelated C
  // pointers. The reader retains the base pointer's ID in both directions.
  assert(read_constant_offset(First, raw_address(Second) - raw_address(First))
         == 's');
  assert(read_constant_offset(Second, raw_address(First) - raw_address(Second))
         == 'f');

  // The target is genuinely readable, but outside the shared ID's bounds.
  assert(read_constant_offset(&Writable, 0) == 0x5a);
  uintptr_t begin = ptsan_get_base(id) & ((UINT64_C(1) << 48) - 1);
  uintptr_t end = begin + ptsan_get_size(id);
  uintptr_t target = raw_address(&Writable);
  assert(target < begin || target >= end);
  if (argc > 1) {
    (void)read_constant_offset(First, raw_address(&Writable) - raw_address(First));
    puts("outside-read-completed");
    return 1;
  }

  puts("shared-constant-access-ok");
  return 0;
}

//--- read.c
// Separate translation unit: the access must use the incoming pointer's ID,
// not a compile-time bound or knowledge of the destination global.
#include <stdint.h>

unsigned char read_constant_offset(const unsigned char *base, intptr_t offset) {
  return *(const volatile unsigned char *)(base + offset);
}
