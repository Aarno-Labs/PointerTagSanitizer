//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// strdup/__strdup must return a TAGGED copy: the InLibcGuard scope has to end
// before tag_with_next_id, which is a NOP while ptsan_in_libc > 0.

// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe 2>&1 | FileCheck %s

// CHECK: OK: strdup tests passed
// CHECK-NOT: BUG:

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char *__strdup(const char *s);

// pointer == is tag-blind under ptsan, so compare the tag bits explicitly
static uint16_t tag_of(const void *p) { return (uint16_t)((uintptr_t)p >> 48); }

static int failures = 0;

static void check_tagged(const char *what, const char *src, const char *dup) {
  if (!dup) {
    fprintf(stderr, "BUG: %s returned NULL\n", what);
    failures++;
    return;
  }
  if (strcmp(src, dup) != 0) {
    fprintf(stderr, "BUG: %s copied wrong contents: \"%s\"\n", what, dup);
    failures++;
  }
  if (tag_of(dup) == 0) {
    fprintf(stderr, "BUG: %s returned untagged pointer %p\n", what, (void *)dup);
    failures++;
  }
}

int main(void) {
  const char *lit = "the quick brown fox";

  char *d1 = strdup(lit);
  check_tagged("strdup(literal)", lit, d1);

  // also dup a heap (tagged) source, and dup the dup
  char *d2 = strdup(d1);
  check_tagged("strdup(heap)", lit, d2);

  char *d3 = __strdup(lit);
  check_tagged("__strdup(literal)", lit, d3);

  free(d3);
  free(d2);
  free(d1);

  if (failures) {
    fprintf(stderr, "FAILED: %d strdup checks\n", failures);
    _exit(1);
  }
  fprintf(stderr, "OK: strdup tests passed\n");
  return 0;
}
