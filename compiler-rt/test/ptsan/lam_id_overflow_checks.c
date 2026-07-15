/* LAM-mode version of id_overflow_checks.c.  This should behave like the
 * 16-bit overflow test, except the runtime ID space ends at 0x7fff.
 */

// REQUIRES: ptsan-lam-u48-runtime
// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_raw -fsanitize=ptsan -g -O2 -fno-discard-value-names -mllvm -ptsan-lam -mllvm -ptsan-runtime-validate -mllvm -ptsan-print-stats -mllvm -ptsan-instrument-globals=0 -I%crt_src/include -o %exe %s
// RUN: ./%exe 2> err.txt
// RUN: FileCheck %s < err.txt

// CHECK: Unreclaimed IDs at exit: 0

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Inputs/ptsan_test_interface.h"

uint32_t N;
#define M 5

extern volatile uint64_t __ptsan_used_ids;

#undef get_id
uint64_t __attribute__((noinline)) get_id(void *ptr) {
  return ((uint64_t)ptr >> 48);
}

#define used_ids_is(n) assert(__ptsan_used_ids == (n))

static void assert_lam_ptr(void *ptr) {
  assert(((uint64_t)ptr >> 63) == 0);
}

static void assert_lam_id(uint64_t id) {
  assert(id <= ptsan_default_id());
}

static void assert_lam_base(uint64_t id) {
  assert((ptsan_get_base((uint16_t)id) >> 63) == 0);
}

int __attribute__((noinline)) do_use(int *buf) {
  return buf[5];
}

#define use(ptr) do_use(ptr)

uint64_t __attribute__((noinline)) funcB(char *buf) {
  char dest[20];
  uint64_t idA = get_id(dest);
  uint64_t idB = get_id(buf);
  memcpy(dest, buf, 20);
  return idA + (idB << 16);
}

uint64_t __attribute__((noinline)) funcA(void) {
  char str[20];
  return funcB(str);
}

void __attribute__((noinline)) test_mallocs(void) {
  int *p1 = (int *)malloc(20 * sizeof(int));
  assert(get_id(p1) != ptsan_default_id());
  assert_lam_id(get_id(p1));
  assert_lam_ptr(p1);
  assert_lam_base(get_id(p1));
  use(p1);

  int *p2 = (int *)malloc(20 * sizeof(int));
  assert(get_id(p2) != ptsan_default_id());
  assert_lam_id(get_id(p2));
  assert_lam_ptr(p2);
  assert_lam_base(get_id(p2));
  use(p2);

  int **os = (int **)malloc(M * sizeof(int *));
  assert(get_id(os) == ptsan_default_id());
  assert_lam_ptr(os);
  assert_lam_base(get_id(os));

  for (int i = 0; i < M; i++) {
    int *buf = (int *)malloc(20 * sizeof(int));
    assert(get_id(buf) == ptsan_default_id());
    assert_lam_ptr(buf);
    assert_lam_base(get_id(buf));
    os[i] = buf;
  }

  free(p1);
  free(p2);

  int *p3 = (int *)malloc(20 * sizeof(int));
  assert(get_id(p3) != ptsan_default_id());
  assert_lam_id(get_id(p3));
  assert_lam_ptr(p3);
  assert_lam_base(get_id(p3));
  use(p3);

  int *p4 = (int *)malloc(20 * sizeof(int));
  assert(get_id(p4) != ptsan_default_id());
  assert_lam_id(get_id(p4));
  assert_lam_ptr(p4);
  assert_lam_base(get_id(p4));
  use(p4);

  int *p5 = (int *)malloc(20 * sizeof(int));
  assert(get_id(p5) == ptsan_default_id());
  assert_lam_ptr(p5);
  assert_lam_base(get_id(p5));
  use(p5);

  free(p5);
  for (int i = 0; i < M; i++)
    free(os[i]);

  int *p6 = (int *)malloc(20 * sizeof(int));
  assert(get_id(p6) == ptsan_default_id());
  assert_lam_ptr(p6);
  assert_lam_base(get_id(p6));
  use(p6);

  free(p3);
  free(p4);
  free(p6);
  free(os);
}

void __attribute__((noinline)) use_some_ids(void) {
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
  assert_lam_id(get_id(b1));
  assert_lam_id(get_id(b2));
  assert_lam_id(get_id(b3));
  assert_lam_id(get_id(b4));
  assert_lam_ptr(b1);
  assert_lam_ptr(b2);
  assert_lam_ptr(b3);
  assert_lam_ptr(b4);
  assert_lam_base(get_id(b1));
  assert_lam_base(get_id(b2));
  assert_lam_base(get_id(b3));
  assert_lam_base(get_id(b4));
}

void __attribute__((noinline)) use_some_ids_with_malloc_inner(void) {
  int b5[30];
  use(b5);
  assert(get_id(b5) != ptsan_default_id());
  assert_lam_id(get_id(b5));
  assert_lam_ptr(b5);
  assert_lam_base(get_id(b5));
}

void __attribute__((noinline)) use_some_ids_with_malloc(void) {
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
  assert_lam_id(get_id(b1));
  assert_lam_id(get_id(b2));
  assert_lam_id(get_id(b3));
  assert_lam_ptr(b1);
  assert_lam_ptr(b2);
  assert_lam_ptr(b3);
  assert_lam_ptr(b4);
  assert_lam_base(get_id(b1));
  assert_lam_base(get_id(b2));
  assert_lam_base(get_id(b3));
  assert_lam_base(get_id(b4));
  free(b2);
  free(b3);
  free(b4);
}

void __attribute__((noinline)) test_stack_ids(void) {
  use_some_ids();
  use_some_ids_with_malloc();
  use_some_ids();
}

int main(void) {
  assert(ptsan_id_bits() == 15);
  assert(ptsan_default_id() == 0x7fff);
  assert(ptsan_get_base(ptsan_default_id()) == 0x7fff000000000000ULL);
  assert_lam_base(ptsan_default_id());

  N = ptsan_num_ids() - 4 - 0x1800;
  int **ptrs = (int **)malloc(sizeof(int *) * N);

  used_ids_is(1);
  assert(get_id(ptrs) == 0x1801);
  assert_lam_ptr(ptrs);
  assert_lam_base(get_id(ptrs));

  int *prebuf = (int *)malloc(20 * sizeof(int));
  use(prebuf);
  assert_lam_id(get_id(prebuf));
  assert_lam_ptr(prebuf);
  assert_lam_base(get_id(prebuf));

  used_ids_is(2);
  free(prebuf);
  used_ids_is(1);

  for (uint32_t i = 0; i < N; i++) {
    int *buf = (int *)malloc(20 * sizeof(int));
    assert_lam_id(get_id(buf));
    assert_lam_ptr(buf);
    ptrs[i] = buf;
  }

  used_ids_is(N + 1);
  assert(get_id(ptrs[N - 2]) != ptsan_default_id());
  assert(get_id(ptrs[N - 2]) <= ptsan_max_id());
  assert_lam_ptr(ptrs[N - 2]);
  assert_lam_base(get_id(ptrs[N - 2]));
  assert(get_id(ptrs[N - 1]) != ptsan_default_id());
  assert(get_id(ptrs[N - 1]) <= ptsan_max_id());
  assert_lam_ptr(ptrs[N - 1]);
  assert_lam_base(get_id(ptrs[N - 1]));

  uint64_t funcID = funcA();
  assert_lam_id(funcID & 0xffff);
  assert_lam_id(funcID >> 16);
  assert_lam_base(funcID & 0xffff);
  assert_lam_base(funcID >> 16);

  test_mallocs();
  used_ids_is(N + 1);

  test_stack_ids();
  used_ids_is(N + 1);

  for (uint32_t i = 0; i < N; i++)
    free(ptrs[i]);

  free(ptrs);
  return 0;
}
