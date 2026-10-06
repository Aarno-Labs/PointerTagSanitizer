/* Check to make sure the ids work right after running out.
 * Less concerned with specific ids and more with is/is not an overflow id
*/

// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -mllvm -ptsan-instrument-globals=0 -O2 -I%crt_src/include -o %exe %s
// RUN: ./%exe 2> err.txt
// RUN: cat err.txt | FileCheck %s

// CHECK: Unreclaimed IDs at exit: 0

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include "Inputs/ptsan_test_interface.h"


uint32_t N;
#define M 5

extern volatile uint64_t __ptsan_used_ids;
extern volatile uint64_t __ptsan_used_global_ids;
uint64_t baseline_used_ids;

#define p(ptr) printf("%-10s id: 0x%lx\n", #ptr, get_id(ptr));

// Need to undef this so we can noinline it for optimization-fighting purposes
#undef get_id
uint64_t __attribute__((noinline)) get_id(void *ptr) {
  return (uint64_t)ptr >> 48;
}

#define use(ptr) \
  p(ptr) \
  do_use(ptr)

#define p_n_ids() \
  printf("%d: used ids: %lu (0x%lx)\n", \
  __LINE__, __ptsan_used_ids, __ptsan_used_ids)

#define used_ids_is(n) \
  assert(__ptsan_used_ids == baseline_used_ids + (n));

int __attribute__((noinline)) do_use(int *buf) {
  return buf[5];
}

uint64_t __attribute__((noinline)) funcB(char *buf) {
  char dest[20];
  uint64_t idA = (uint64_t)dest >> 48;
  uint64_t idB = (uint64_t)buf >> 48;
  memcpy(dest, buf, 20);
  return idA + (idB << 16);
}

uint64_t __attribute__((noinline)) funcA() {
  char str[20];
  return funcB(str);
}

void __attribute__((noinline)) test_mallocs() {
  printf("----------------test mallocs-------------\n");
  // use up the last two ids
  int *p1 = malloc(20 * sizeof(int));
  assert(get_id(p1) != ptsan_default_id());
  use(p1);

  int *p2 = malloc(20 * sizeof(int));
  assert(get_id(p2) != ptsan_default_id());
  use(p2);

  // At this point, we should be out of ids
  int **os = malloc(M * sizeof(int *));
  assert(get_id(os) == ptsan_default_id());

  for(int i = 0; i < M; i++) {
    int *buf = (int *)malloc(20 * sizeof(int));

    assert(get_id(buf) == ptsan_default_id());

    os[i] = buf;
  }

  // Now free the last two ids
  free(p1);
  free(p2);

  // So this should be a non-overflow id
  int *p3 = malloc(20 * sizeof(int));
  assert(get_id(p3) != ptsan_default_id());
  use(p3);

  // So this should be a non-overflow id
  int *p4 = malloc(20 * sizeof(int));
  assert(get_id(p4) != ptsan_default_id());
  use(p4);

  // So this should be an overflow id
  int *p5 = malloc(20 * sizeof(int));
  assert(get_id(p5) == ptsan_default_id());
  use(p5);

  free(p5);
  for(int i = 0; i < M; i++) {
    free(os[i]);
  }

  int *p6 = malloc(20 * sizeof(int));
  assert(get_id(p6) == ptsan_default_id());
  use(p6);

  free(p3);
  free(p4);
  free(p6);
  free(os);
  // Should end with two free ids
}

void __attribute__((noinline)) use_some_ids() {
  int b1[30];
  int b2[30];
  int b3[30];
  int b4[30];

  use(b1);
  use(b2);
  use(b3);
  use(b4);
  assert(get_id(b1) != ptsan_default_id());
  assert(get_id(b2) != ptsan_default_id());
  assert(get_id(b3) != ptsan_default_id());
  assert(get_id(b4) != ptsan_default_id());
}

void __attribute__((noinline)) use_some_ids_with_malloc_inner() {
  int b5[30];
  use(b5);
  assert(get_id(b5) != ptsan_default_id());
}

void __attribute__((noinline)) use_some_ids_with_malloc() {
  int b1[30];

  int *b2 = (int *)malloc(sizeof(int) * 10);
  int *b3 = (int *)malloc(sizeof(int) * 10);
  int *b4 = (int *)malloc(sizeof(int) * 10);
  use(b2);
  use(b3);
  use(b4);

  use_some_ids_with_malloc_inner();

  assert(get_id(b1) != ptsan_default_id());
  assert(get_id(b2) != ptsan_default_id());
  assert(get_id(b3) != ptsan_default_id());
  assert(get_id(b4) == ptsan_default_id());
  free(b2);
  free(b3);
  free(b4);
}

void __attribute__((noinline)) test_stack_ids() {
  printf("----------------test stack ids-------------\n");
  use_some_ids();
  use_some_ids_with_malloc();
  use_some_ids();
}

int main(int argc, char *argv[]) {
  baseline_used_ids = __ptsan_used_global_ids;
  // `- 4` because ptrs eats an ID, and max_id is reserved, and we want
  // two to play with
  // - 0x1800 because those are reserved for stack ids
  N = ptsan_num_ids() - 4 - 0x1800 - baseline_used_ids;
  int ** ptrs = (int **) malloc(sizeof(int *) * N);

  used_ids_is(1);

  p(ptrs)

  int *prebuf = (int *)malloc(20 * sizeof(int));
  use(prebuf);
  uint64_t prebufID = (uint64_t)prebuf >> 48;

  used_ids_is(2);
  free(prebuf);


  used_ids_is(1);
  // Use all but two of the ids
  for (int i = 0; i < N; i++) {
    int *buf = (int *)malloc(20 * sizeof(int));

    ptrs[i] = buf;
  }

  p_n_ids();
  used_ids_is(N + 1);

  test_mallocs();

  p_n_ids();
  used_ids_is(N + 1);

  test_stack_ids();

  p_n_ids();
  used_ids_is(N + 1);

  printf("-----------------done--------------------\n");

  for (int i = 0; i < N; i++) {
    free(ptrs[i]);
  }

  free(ptrs);

  return 0;
}
