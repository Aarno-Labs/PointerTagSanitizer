//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Model the offset/reconstruction used by the legacy obstack_free macro, without
// depending on which obstack.h the host supplies. Keep helpers in a separate TU
// so the non-null target is not constant-propagated into the macro expansion.
// NULL and cross-allocation pointer subtraction are not portable ISO C: these
// cases test compatibility with the legacy flat-address implementation. The
// uintptr_t control uses unsigned arithmetic; LLVM folds its reconstruction to
// the original target before instrumentation.
// RUN: split-file %s %t.dir
// RUN: %clang_raw -O3 %t.dir/helper.c %t.dir/main.c -o %t.native
// RUN: %run %t.native 0
// RUN: %run %t.native 1
// RUN: %run %t.native 2
// RUN: %run %t.native 3
// RUN: %run %t.native 4
// RUN: %clang_ptsan_id32 -O3 -I%S -mllvm -ptsan-clear-ids-on-arith=0 %t.dir/helper.c %t.dir/main.c -o %t.keep
// RUN: %run %t.keep 0 check-id
// RUN: %run %t.keep 1 check-id
// RUN: %run %t.keep 2 check-id
// RUN: %run %t.keep 3 check-id
// RUN: %run %t.keep 4 check-id
// RUN: %clang_ptsan_id32 -O3 -I%S -mllvm -ptsan-clear-ids-on-arith=1 %t.dir/helper.c %t.dir/main.c -o %t.strip
// RUN: %run %t.strip 0 check-id
// RUN: %run %t.strip 1 check-id
// RUN: %run %t.strip 2 check-id
// RUN: %run %t.strip 3 check-id
// RUN: %run %t.strip 4 check-id

//--- model.h
#include <stddef.h>
#include <stdint.h>
struct arena {
  char *chunk;
  char *limit;
  char *next;
  ptrdiff_t temp;
};
int fallback(struct arena *, char *);
int rewind_null(struct arena *);
int rewind_pointer(struct arena *, char *);
int rewind_integer(struct arena *, char *);

//--- helper.c
#include "model.h"

#define REWIND(a, target) do { \
  (a)->temp = (target) - (a)->chunk; \
  if ((a)->temp > 0 && (a)->temp < (a)->limit - (a)->chunk) { \
    (a)->next = (a)->chunk + (a)->temp; \
    return 0; \
  } \
  return fallback((a), (a)->chunk + (a)->temp); \
} while (0)

int rewind_null(struct arena *a) {
  REWIND(a, (char *)0);
}

int rewind_pointer(struct arena *a, char *target) {
  REWIND(a, target);
}

int rewind_integer(struct arena *a, char *target) {
  uintptr_t delta = (uintptr_t)target - (uintptr_t)a->chunk;
  a->temp = delta;
  return fallback(a, (char *)((uintptr_t)a->chunk + delta));
}

//--- main.c
#include "model.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef PTSAN_TEST_32_BIT_IDS
#include "Inputs/ptsan_test_interface.h"
#endif

static char *observed;
static int used_fallback;
int fallback(struct arena *a, char *p) {
  used_fallback = 1;
  observed = p;
  return 0;
}

int main(int argc, char **argv) {
  assert(argc == 2 || argc == 3);
  char *older = malloc(128);
  char *current = malloc(128);
  assert(older && current);
  struct arena a = {current, current + 128, current, 0};
  int scenario = atoi(argv[1]);
  char *expected = NULL;
  if (scenario == 0) {
    // Literal NULL must reach the fallback as NULL, not an unrelated address.
    rewind_null(&a);
  } else if (scenario == 1) {
    // Rewinding within this chunk must preserve both the address and its ID.
    expected = current + 16;
    rewind_pointer(&a, expected);
  } else if (scenario == 2) {
    // The fallback must receive the older chunk's address and ID.
    expected = older + 16;
    rewind_pointer(&a, expected);
  } else if (scenario == 3) {
    // Unsigned-integer cancellation must retain the older pointer unchanged.
    expected = older + 16;
    rewind_integer(&a, expected);
  } else {
    // The same unsigned-integer control, with a null target.
    assert(scenario == 4);
    rewind_integer(&a, NULL);
  }
  assert(used_fallback == (scenario != 1));
  char *got = used_fallback ? observed : a.next;
#ifdef PTSAN_TEST_32_BIT_IDS
  int address_equal = ptsan_get_address(got) == ptsan_get_address(expected);
  int id_equal = get_id(got) == get_id(expected);
  printf("case=%d address_equal=%d id_equal=%d got_id=%u expected_id=%u\n",
         scenario, address_equal, id_equal, get_id(got), get_id(expected));
  int failed = !address_equal || (argc == 3 && !id_equal);
#else
  printf("case=%d address_equal=%d\n", scenario, got == expected);
  int failed = got != expected;
#endif
  free(current);
  free(older);
  return failed;
}
