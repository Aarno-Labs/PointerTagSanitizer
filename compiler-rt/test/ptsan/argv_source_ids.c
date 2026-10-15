//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %clang_ptsan -O2 -mllvm -ptsan-abort-on-error -o %t %s
// RUN: %t 2>&1 | FileCheck %s --check-prefix=GOOD
// RUN: not %t bad 2>&1 | FileCheck %s --check-prefix=BAD
// RUN: %clang_ptsan -O2 -mllvm -ptsan-no-opts \
// RUN:   -mllvm -ptsan-abort-on-error -o %t.noopts %s
// RUN: %t.noopts 2>&1 | FileCheck %s --check-prefix=GOOD
// RUN: not %t.noopts bad 2>&1 | FileCheck %s --check-prefix=BAD

// GOOD-NOT: Found bad
// GOOD: argv-source-ids-ok
// GOOD-NOT: Found bad
// BAD: Found bad POINT access

// argv is a raw caller-provided source, not an ordinary tagged argument.
// Its direct accesses and escaping uses must agree about the allocation.
// In particular, rebuilding a check range from stale ScalarEvolution state
// must not put that range back on raw argv and lose its assigned ID.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

__attribute__((noinline)) static uintptr_t pointer_bits(char **p) {
  return (uintptr_t)p;
}

int main(int argc, char **argv) {
  uintptr_t bits = pointer_bits(argv);
  assert(bits >> 48);
  assert(pointer_bits(argv + 1) >> 48 == bits >> 48);
  assert(argv[0] != NULL);
  assert(argv[argc] == NULL);
  // A volatile load keeps the access itself, even though only its execution
  // and PTSan report matter. argc+1 is outside the argv array, including its
  // terminating null slot.
  if (argc > 1) {
    char *value = ((char *volatile *)argv)[argc + 1];
    return value != NULL;
  }
  puts("argv-source-ids-ok");
  return 0;
}
