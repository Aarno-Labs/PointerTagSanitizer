//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// CHECK: Found bad POINT access

//--- main.c
#include "Inputs/ptsan_test_interface.h"
#include <assert.h>
#include <stdlib.h>

static int Value = 42;
int read_value(const volatile int *p);

#ifdef EARLY
// Musl completes PTSan initialization at constructor priority 2, after global
// proxies receive IDs. Keep the early incoming ID-zero access in a separate
// translation unit.
static int EarlyValue;
static uint64_t EarlyZeroSize;
static uint64_t EarlyDefaultSize;
static uint16_t EarlyID;

__attribute__((constructor(0))) static void beforeGlobalIDs(void) {
  EarlyZeroSize = ptsan_get_size(0);
  EarlyDefaultSize = ptsan_get_size(ptsan_default_id());
  EarlyID = get_id(&Value);
  EarlyValue = read_value(&Value);
}
#endif

int main(int argc, char **argv) {
  (void)argv;
#ifdef EARLY
  assert(EarlyZeroSize != 0 && EarlyDefaultSize != 0);
  assert(EarlyID == 0 && EarlyValue == 42);
#endif
  assert(ptsan_get_size(0) == 0);
  assert(ptsan_get_size(ptsan_default_id()) == 0);
  assert(get_id(&Value) != 0);
  assert(read_value(&Value) == 42);

  if (argc > 1) {
    // The memory is valid, but both fallback IDs must now be rejected.
    uintptr_t raw = (uintptr_t)&Value & ((UINT64_C(1) << 48) - 1);
    if (argc > 2)
      raw |= ptsan_default_id() << 48;
    return read_value((const volatile int *)raw);
  }
  // Keep initialization checks independent of libc teardown.
  _Exit(0);
}

//--- read.c
int read_value(const volatile int *p) {
  return *p;
}
