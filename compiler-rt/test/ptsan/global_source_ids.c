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
// GOOD: global-source-ids-ok
// GOOD-NOT: Found bad
// BAD: Found bad POINT access

// Direct memory operands use raw symbols; escaping pointers need the proxy's
// ID. Cover derived addresses, selection between different globals, and a
// data alias. Scalars and each thread's TLS objects also receive IDs.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

char bytes[16];
char other[16];
extern char alias[16] __attribute__((alias("bytes")));
char scalar;
_Thread_local char tls_bytes[16];
static volatile unsigned choice;

__attribute__((noinline)) static unsigned id_of(const void *p) {
  return (uintptr_t)p >> 48;
}

__attribute__((noinline)) static char *select_global(unsigned which) {
  return which ? other + 4 : bytes + 4;
}

__attribute__((noinline)) static char read_at(char *p, unsigned index) {
  return *(volatile char *)(p + index);
}

int main(int argc, char **argv) {
  bytes[4] = 7;
  other[4] = 9;
  unsigned id = id_of(bytes);
  assert(id && id_of(alias) == id && id_of(bytes + 4) == id);
  assert(id_of(other) && id_of(other) != id);
  unsigned scalar_id = id_of(&scalar);
  assert(scalar_id && scalar_id != id && scalar_id != id_of(other));
  assert(id_of(tls_bytes) != 0 && id_of(tls_bytes) != id);
  tls_bytes[0] = 3;
  assert(read_at(tls_bytes, 0) == 3);
  char *p = select_global(choice);
  assert(id_of(p) == id && read_at(p, 0) == 7);
  choice = 1;
  p = select_global(choice);
  assert(id_of(p) == id_of(other) && read_at(p, 0) == 9);
  if (argc > 1) {
    return read_at(p, 12);
  }
  puts("global-source-ids-ok");
  return 0;
}
