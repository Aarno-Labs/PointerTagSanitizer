//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: split-file %s %t
// RUN: %clang_ptsan -I%S -O2 -fno-pie -no-pie %t/main.c %t/owners.c %t/target.c -o %t/nopie
// RUN: %t/nopie
// RUN: %t/nopie protect
// RUN: %clang_ptsan -I%S -O2 -fuse-ld=bfd %t/empty.c -o %t/empty-bfd
// RUN: %t/empty-bfd
// RUN: %if lld-available %{ %clang_ptsan -I%S -O2 -fuse-ld=lld %t/empty.c -o %t/empty-lld %}
// RUN: %if lld-available %{ %t/empty-lld %}
// RUN: %clang_ptsan_id32 -I%S -O2 -fno-pie -no-pie \
// RUN:   %t/main.c %t/owners.c %t/target.c -o %t/id32
// RUN: %t/id32
// RUN: %t/id32 protect

// Separate modules prevent constant folding of the loads and exercise replay
// ordering and deduplication of the final sealing constructor.
//--- empty.c
int main(void) { return 0; }

//--- owners.c
extern int target[4];
int *const slot = target;
struct Table { int *elements[2]; int count; };
const struct Table table = {{target + 1, target + 3}, 2};

//--- target.c
int target[4] = {11, 22, 33, 44};
extern int *const slot;
int *const *const indirect = &slot;

//--- main.c
#include <assert.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include "Inputs/ptsan_test_interface.h"

extern int target[4];
extern int *const slot;
extern int *const *const indirect;
struct Table { int *elements[2]; int count; };
extern const struct Table table;

__attribute__((constructor)) static void check_initializers(void) {
  uintptr_t id = get_id(target);
  assert(id && id != ptsan_default_id());
  assert(get_id(slot) == id);
  assert(get_id(table.elements[0]) == id);
  assert(get_id(table.elements[1]) == id);
  assert(get_id(indirect) == get_id(&slot));
  assert(*indirect == slot);
  assert(*slot == 11 && *table.elements[0] == 22);
  assert(*table.elements[1] == 44 && table.count == 2);
}

// Deliberately bypass instrumentation to test actual page permissions, not a
// PTSan bounds check. Success requires the attempted write to fault.
__attribute__((disable_sanitizer_instrumentation))
static void protected_write(int signal_number) {
  _Exit(signal_number == SIGSEGV ? 0 : 1);
}

__attribute__((noinline, disable_sanitizer_instrumentation))
static void overwrite(void *pointer) {
  *(volatile uintptr_t *)ptsan_get_address(pointer) = 0;
}

int main(int argc, char **argv) {
  target[0] = 55; // Neighbouring ordinary globals must remain writable.
  assert(*slot == 55);
  if (argc == 2) {
    signal(SIGSEGV, protected_write);
    overwrite((void *)&slot);
    return 1;
  }
  if (argc > 2) {
    return ((volatile int *)slot)[4];
  }
  return 0;
}
