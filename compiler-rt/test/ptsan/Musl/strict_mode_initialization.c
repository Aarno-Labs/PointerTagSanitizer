//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: split-file %s %t.split
// RUN: %ptsan_musl_strict -DEARLY -I%S/.. -o %t \
// RUN:   %t.split/main.c %t.split/read.c
// RUN: %run %t
// RUN: %ptsan_musl_strict -DEARLY -DBAD_EARLY -mllvm -ptsan-abort-on-error -I%S/.. -o %t.bad %t.split/main.c %t.split/read.c
// RUN: not %run %t.bad 2>&1 | FileCheck %s
// RUN: %ptsan_musl_strict -DEARLY -DBAD_EARLY_LIBC -mllvm -ptsan-abort-on-error -I%S/.. -o %t.libc %t.split/main.c %t.split/read.c
// RUN: not %run %t.libc 2>&1 | FileCheck %s --check-prefix=LIBC
// RUN: %ptsan_musl_strict -O2 \
// RUN:   -mllvm -ptsan-abort-on-error -I%S/.. -o %t \
// RUN:   %t.split/main.c %t.split/read.c
// RUN: %run %t
// RUN: not %run %t zero 2>&1 | FileCheck %s
// RUN: not %run %t default id 2>&1 | FileCheck %s

// CHECK: Found bad POINT access
// LIBC: Found bad read access

//--- main.c
#include "Inputs/ptsan_test_interface.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static int Value = 42;
int read_value(const volatile int *p);

#ifdef EARLY
// Musl globals already have IDs, and fallback bounds are empty even before
// libc-dependent runtime setup finishes. Keep accesses in a separate TU.
static int EarlyValue;
static uint64_t EarlyZeroSize;
static uint64_t EarlyDefaultSize;
static uint32_t EarlyID;

__attribute__((constructor(0))) static void beforeRuntimeFinish(void) {
  EarlyZeroSize = ptsan_get_size(0);
  EarlyDefaultSize = ptsan_get_size(ptsan_default_id());
  EarlyID = get_id(&Value);
#if defined(BAD_EARLY_LIBC)
  static const char Text[] = "early";
  size_t (*volatile scan)(const char *) = strlen;
  EarlyValue = scan((const char *)ptsan_get_address(Text));
#elif defined(BAD_EARLY)
  EarlyValue = read_value((const volatile int *)ptsan_get_address(&Value));
#else
  EarlyValue = read_value(&Value);
#endif
}
#endif

int main(int argc, char **argv) {
  (void)argv;
#ifdef EARLY
  assert(EarlyZeroSize == 0 && EarlyDefaultSize == 0);
  assert(EarlyID != 0 && EarlyValue == 42);
#endif
  assert(ptsan_get_size(0) == 0);
  assert(ptsan_get_size(ptsan_default_id()) == 0);
  assert(get_id(&Value) != 0);
  assert(read_value(&Value) == 42);

  if (argc > 1) {
    // The memory is valid, but both fallback IDs must now be rejected.
    uintptr_t raw = ptsan_get_address(&Value);
    if (argc > 2) {
      if (ptsan_id_bits() >= 31) {
        raw = UINT64_C(0x8000000000000000) | (ptsan_default_id() << 32) |
              (uint32_t)raw;
      } else {
        raw |= ptsan_default_id() << 48;
      }
    }
    return read_value((const volatile int *)raw);
  }
  // Keep the initialization probe independent of finalizer execution.
  _Exit(0);
}

//--- read.c
int read_value(const volatile int *p) {
  return *p;
}
