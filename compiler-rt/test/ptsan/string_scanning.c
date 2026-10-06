//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %clang_ptsan -O2 -fno-builtin -mllvm -ptsan-abort-on-error %s -o %t
// RUN: %clang_ptsan -O2 -fno-builtin -mllvm -ptsan-abort-on-error -DBAD_SOURCE %s -o %t.bad
// RUN: args=''; for i in 0 1 2 3 4 5 6 7 8 9 10 11 12; do \
// RUN:   %run %t $args && \
// RUN:   not %run %t.bad $args 2>&1 | FileCheck %s || exit 1; \
// RUN:   args="$args x"; \
// RUN: done
// RUN: %clang_ptsan -O2 -fno-builtin -mllvm -ptsan-abort-on-error -DZERO_ID %s -o %t.zero
// RUN: %clang_ptsan -O2 -fno-builtin -mllvm -ptsan-abort-on-error -DDEFAULT_ID %s -o %t.default
// RUN: args=''; for i in 0 1 2 3 4 5 6 7 8 9 10 11 12; do \
// RUN:   %run %t.zero $args && %run %t.default $args || exit 1; \
// RUN:   args="$args x"; \
// RUN: done

// Read diagnostics must describe the first invalid byte, not a range checked
// after an unchecked strlen/search. Use argument count without reading argv.
// CHECK: Found bad read access: #0. Function ID: 0xdeadbeef
// CHECK: (Access size 0x1)

#define _GNU_SOURCE
#include "Inputs/ptsan_test_interface.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static void check_pointer(const void *actual, const void *expected) {
  assert(actual == expected);
  assert(get_id(actual) == get_id(expected));
}

int main(int argc, char **argv) {
  (void)argv;
  char storage[2] = {'a', '\0'};
#ifdef BAD_SOURCE
  storage[1] = 'a';
#endif
  char *source = storage;
#if defined(ZERO_ID) || defined(DEFAULT_ID)
  uintptr_t address = (uintptr_t)source & ((UINT64_C(1) << 48) - 1);
#ifdef DEFAULT_ID
  address |= (uintptr_t)ptsan_default_id() << 48;
#endif
  source = (char *)address;
#endif
  char dest[32] = {0};
  char comparison[16];
  for (size_t i = 0; i < sizeof(comparison) - 1; ++i) {
    comparison[i] = 'a';
  }
  comparison[15] = '\0';

  // Zero-length operations must remain valid even for strict reserved IDs.
  assert(strnlen(source, 0) == 0);
  assert(memchr(source, 'z', 0) == NULL);
  assert(strncmp(source, comparison, 0) == 0);

  switch (argc - 1) {
  case 0:
    assert(strlen(source) == 1);
    break;
  case 1:
    assert(strnlen(source, 16) == 1);
    break;
  case 2:
    check_pointer(strchrnul(source, 'z'), source + 1);
    break;
  case 3:
    // The maximum extends beyond the object; an early match is still valid.
    check_pointer(memchr(source, '\0', 16), source + 1);
    break;
  case 4:
    assert(strcmp(source, comparison) < 0);
    break;
  case 5:
    assert(strncmp(source, comparison, 16) < 0);
    break;
  case 6:
    check_pointer(stpcpy(dest, source), dest + 1);
    break;
  case 7:
    check_pointer(stpncpy(dest, source, 16), dest + 1);
    assert(dest[15] == '\0');
    break;
  case 8:
    check_pointer(strncpy(dest, source, 16), dest);
    assert(dest[15] == '\0');
    break;
  case 9:
    check_pointer(strcpy(dest, source), dest);
    break;
  case 10:
    check_pointer(strcat(dest, source), dest);
    break;
  case 11:
    check_pointer(strncat(dest, source, 16), dest);
    break;
  case 12:
    assert(strchr(source, 'z') == NULL);
    break;
  default:
    _Exit(2);
  }
  // Avoid unrelated strict-mode allocation and teardown paths.
  _Exit(0);
}
