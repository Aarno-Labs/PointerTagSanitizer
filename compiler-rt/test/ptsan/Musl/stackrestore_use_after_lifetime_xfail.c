//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// REQUIRES: x86_64-target-arch
// XFAIL: *
// RUN: %ptsan_musl_strict -O2 \
// RUN:   -mllvm -ptsan-stack-temporal-safety=1 \
// RUN:   -mllvm -ptsan-abort-on-error -o %t %s
// RUN: %t 2>&1 | FileCheck %s --check-prefix=VALID
// RUN: not %t stale 2>&1 | FileCheck %s --check-prefix=UAL
// VALID: stack storage reused
// UAL: stack storage reused
// UAL: Found bad

// stackrestore releases the first VLA, but PTSan retains its ID/bounds until
// function exit. A second VLA reuses that storage with a different ID. Writing
// through the expired pointer currently corrupts the live VLA without a report.
// Neither alloca repeats: this is independent of loop-alloca support.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static volatile unsigned length = 37;
static volatile uintptr_t escaped;

__attribute__((noinline)) static void remember(char *p) {
  escaped = (uintptr_t)p;
}

__attribute__((noinline)) static void write_byte(char *p, char value) {
  *(volatile char *)p = value;
}

int main(int argc, char **argv) {
  unsigned n = length;
  {
    char expired[n];
    write_byte(expired, 'A');
    remember(expired);
  }
  {
    char active[n];
    write_byte(active, 'B');
    uintptr_t old = escaped;
    uintptr_t current = (uintptr_t)active;
    uintptr_t address_mask = ((uintptr_t)1 << 48) - 1;
    assert((old & address_mask) == (current & address_mask));
    assert((old >> 48) != 0 && (old >> 48) != 0xffff);
    assert((current >> 48) != 0 && (current >> 48) != 0xffff);
    assert((old >> 48) != (current >> 48));
    fprintf(stderr, "stack storage reused\n");
    if (argc > 1) {
      write_byte((char *)old, 'X');
      assert(*(volatile char *)active == 'X');
      fprintf(stderr, "uncaught stale write corrupted live buffer\n");
    } else {
      assert(*(volatile char *)active == 'B');
    }
  }
  return 0;
}
