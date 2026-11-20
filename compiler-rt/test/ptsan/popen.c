//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe > out.txt 2>&1
// RUN: cat out.txt | FileCheck %s

// Regression for the popen() interceptor with a ptsan-tagged `command`.
//
// The `command` string handed to popen() below is a ptsan-tagged heap
// allocation (malloc carries an id in the high 16 bits). The interceptor must
// not fault on it: the forked shell runs in uninstrumented libc, and the
// shadow-range read (internal_strlen / COMMON_INTERCEPTOR_READ_RANGE) walks the
// string.
//
// NOTE: the interceptor's `command = clear_id(command)` is redundant (verified
// by a runtime differential: removing it, rebuilding the rt, and rerunning this
// test still passes). REAL(popen) is reached via doPTSanCall, which already does
// f(stripID(args)...), and the rt is compiled -ptsan-strip-ids so the
// READ_RANGE/internal_strlen path masks tags on deref. This test guards that the
// tagged-command path stays working whether or not that line is present.
//
// `type` is a plain literal here so this isolates the `command` arg.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Return a tagged heap copy of s (the returned pointer carries a ptsan id).
__attribute__((noinline)) static char *tagged_dup(const char *s) {
  size_t n = strlen(s) + 1;
  char *p = (char *)malloc(n);
  assert(p);
  memcpy(p, s, n);
  return p;
}

int main(void) {
  // --- read mode: child writes two lines, we read them back ---
  char *cmd = tagged_dup("printf 'line1\\nline2\\n'");
  FILE *fp = popen(cmd, "r");
  assert(fp);
  char buf[256];
  while (fgets(buf, sizeof(buf), fp))
    fputs(buf, stdout);
  // CHECK: line1
  // CHECK-NEXT: line2
  if (pclose(fp) != 0) {
    perror("pclose(read)");
    return 1;
  }
  free(cmd);
  printf("[OK] popen read mode\n");
  // CHECK: [OK] popen read mode

  // --- write mode: we feed the child through the pipe ---
  char *cmd2 = tagged_dup("cat > /dev/null");
  FILE *wp = popen(cmd2, "w");
  assert(wp);
  fputs("hello\n", wp);
  if (pclose(wp) != 0) {
    perror("pclose(write)");
    return 1;
  }
  free(cmd2);
  printf("[OK] popen write mode\n");
  // CHECK: [OK] popen write mode

  return 0;
}
