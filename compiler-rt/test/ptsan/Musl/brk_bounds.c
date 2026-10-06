//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict -o %t %s
// RUN: %run %t
// RUN: not %run %t end 2>&1 | FileCheck %s
// RUN: not %run %t shrunk 2>&1 | FileCheck %s
// CHECK: Found bad POINT access

// Use musl's fixed-argument syscall entry: public brk() rejects requests,
// and variadic syscall() has a separate strict-mode va_arg limitation.
// Avoid malloc and stdio while this test owns the process break.
#define _GNU_SOURCE
#include "../Inputs/ptsan_test_interface.h"
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/syscall.h>

extern long __syscall_cp(long, long, long, long, long, long, long);

static char *set_break(char *address) {
  return (char *)__syscall_cp(SYS_brk, (long)address, 0, 0, 0, 0, 0);
}

__attribute__((noinline)) static void touch(char *p) {
  *(volatile char *)p = 42;
}

int main(int argc, char **argv) {
  char *start = set_break(0);
  uint16_t id = get_id(start);
  if (!id || id == ptsan_default_id()) {
    _Exit(1);
  }

  uint64_t old_size = ptsan_get_size(id);
  char *end = start + 8192;
  if (set_break(end) != end || set_break(0) != end ||
      ptsan_get_size(id) != old_size + 8192) {
    _Exit(2);
  }

  // These pointers derive from the initial query, not the growth result.
  touch(start);
  touch(end - 1);
  if (argc > 1 && argv[1][0] == 'e') {
    touch(end);
  }

  // Shrink by one byte, leaving the page mapped. The old pointer must now
  // fail a bounds check, not merely fault because the page was unmapped.
  char *smaller = end - 1;
  if (set_break(smaller) != smaller ||
      ptsan_get_size(id) != old_size + 8191) {
    _Exit(3);
  }
  touch(smaller - 1);
  if (argc > 1) {
    touch(smaller);
  }

  // Force growth to fail without requesting a huge allocation. A failed
  // syscall returns the current tagged break and preserves its bounds.
  struct rlimit limit;
  if (__syscall_cp(SYS_prlimit64, 0, RLIMIT_DATA, 0, (long)&limit, 0, 0)) {
    _Exit(4);
  }
  rlim_t old_limit = limit.rlim_cur;
  limit.rlim_cur = 0;
  if (__syscall_cp(SYS_prlimit64, 0, RLIMIT_DATA, (long)&limit, 0, 0, 0)) {
    _Exit(5);
  }
  if (set_break(end + 8192) != smaller ||
      ptsan_get_size(id) != old_size + 8191) {
    _Exit(6);
  }
  touch(smaller - 1);
  limit.rlim_cur = old_limit;
  if (__syscall_cp(SYS_prlimit64, 0, RLIMIT_DATA, (long)&limit, 0, 0, 0)) {
    _Exit(7);
  }
  if (set_break(start) != start || ptsan_get_size(id) != old_size) {
    _Exit(8);
  }
  _Exit(0);
}
