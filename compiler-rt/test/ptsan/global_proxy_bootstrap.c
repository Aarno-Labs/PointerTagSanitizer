//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Before priority-1 proxy constructors, instrumented uses must see the real
// global address with ID zero, not null. Afterwards they must see its ID.
// Glibc-mode validation assumes user code runs after PTSan's constructors;
// this deliberately runs before them. Use the non-validation runtime here.
// RUN: %clang_raw -fsanitize=ptsan -O2 -o %t %s
// RUN: %run %t

#include "Inputs/ptsan_test_interface.h"
#include <assert.h>

static int Value = 42;
static const char Text[] = "bootstrap";
static int EarlyValue;
static int EarlyCharacter;
static uint16_t EarlyValueID;
static uint16_t EarlyTextID;
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
  assert(EarlyValueID == 0 && EarlyTextID == 0);
  assert(EarlyAddress == ((uintptr_t)&Value & ((UINT64_C(1) << 48) - 1)));
  assert(get_id(&Value) != 0 && get_id(Text) != 0);
  assert(readValue(&Value) == 42 && readText(Text) == 'b');
  return 0;
}
