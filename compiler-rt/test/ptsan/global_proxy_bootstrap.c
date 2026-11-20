//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Musl initializes proxies before TLS; even priority-0 constructors see IDs.
// On host libc, before priority-1 proxy constructors, uses must see the real
// global address with ID zero, not null. Afterwards they must see its ID.
// Glibc-mode validation assumes user code runs after PTSan's constructors;
// this deliberately runs before them. Use the non-validation runtime here.
// RUN: %clang_ptsan_plain -O2 -o %t %s
// RUN: %run %t

// RUN: %clang_ptsan_plain -fsanitize-ptsan-32-bit-ids -DPTSAN_TEST_32_BIT_IDS -O2 -o %t %s
// RUN: %run %t

#include "Inputs/ptsan_test_interface.h"
#include <assert.h>

static int Value = 42;
static const char Text[] = "bootstrap";
static int EarlyValue;
static int EarlyCharacter;
static PTSanID EarlyValueID;
static PTSanID EarlyTextID;
static volatile uintptr_t EarlyAddress;

__attribute__((noinline)) static int readValue(const volatile int *p) {
  return *p;
}

__attribute__((noinline)) static int readText(const volatile char *p) {
  return *p;
}

__attribute__((constructor(0))) static void beforeGlobalIDs(void) {
  EarlyValue = readValue(&Value);
  EarlyCharacter = readText(Text);
  EarlyValueID = get_id(&Value);
  EarlyTextID = get_id(Text);
  EarlyAddress = (uintptr_t)&Value;
}

int main(void) {
  assert(EarlyValue == 42);
  assert(EarlyCharacter == 'b');
#ifdef PTSAN_EARLY_GLOBALS
  assert(EarlyValueID == get_id(&Value) && EarlyTextID == get_id(Text));
  assert(EarlyAddress == (uintptr_t)&Value);
#else
  assert(EarlyValueID == 0 && EarlyTextID == 0);
  assert(EarlyAddress == ptsan_get_address(&Value));
#endif
  assert(get_id(&Value) != 0 && get_id(Text) != 0);
  assert(readValue(&Value) == 42 && readText(Text) == 'b');
  return 0;
}
