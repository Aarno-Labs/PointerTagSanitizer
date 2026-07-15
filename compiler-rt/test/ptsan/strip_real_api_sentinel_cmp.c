// RUN: %clang_raw -fsanitize=ptsan -O1 -g -fno-discard-value-names \
// RUN:   -mllvm -ptsan-reoptimize=0 -o %t.default %s
// RUN: %t.default | FileCheck %s
// RUN: %clang_raw -fsanitize=ptsan -O1 -g -fno-discard-value-names \
// RUN:   -mllvm -ptsan-reoptimize=0 -mllvm -ptsan-clear-ids-on-cmp=0 \
// RUN:   -o %t.no-cmp %s
// RUN: %t.no-cmp | FileCheck %s
// RUN: %clang_raw -fsanitize=ptsan -O1 -g -fno-discard-value-names \
// RUN:   -mllvm -ptsan-reoptimize=0 -mllvm -ptsan-clear-ids-on-arith=0 \
// RUN:   -o %t.no-arith %s
// RUN: %t.no-arith | FileCheck %s
// RUN: %clang_raw -fsanitize=ptsan -O1 -g -fno-discard-value-names \
// RUN:   -mllvm -ptsan-reoptimize=0 -mllvm -ptsan-clear-ids-on-cmp=0 \
// RUN:   -mllvm -ptsan-clear-ids-on-arith=0 -o %t.no-cmp-arith %s
// RUN: %t.no-cmp-arith | FileCheck %s
//
// These are the real-world sentinel comparison shapes PTSan must preserve
// without adding dynamic checks in strip lowering.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

#ifndef MAP_ANONYMOUS
#  define MAP_ANONYMOUS MAP_ANON
#endif

__attribute__((noinline)) void *get_zero(void) {
  return (void *)(uintptr_t)0;
}

__attribute__((noinline)) void *get_minus_one(void) {
  return (void *)(uintptr_t)-1;
}

__attribute__((noinline)) void *get_one(void) {
  return (void *)(uintptr_t)1;
}

__attribute__((noinline)) void *get_false_pointer(void) {
  return (void *)(uintptr_t)0;
}

static volatile size_t HugeSize = (size_t)-1;

#define CHECK_TRUE(Name, Expr)                                                \
  do {                                                                        \
    if (!(Expr)) {                                                            \
      printf("FAIL %s\n", Name);                                             \
      Failed = 1;                                                             \
    }                                                                         \
  } while (0)

#define CHECK_FALSE(Name, Expr)                                               \
  do {                                                                        \
    if (Expr) {                                                               \
      printf("FAIL %s\n", Name);                                             \
      Failed = 1;                                                             \
    }                                                                         \
  } while (0)

int main(void) {
  size_t Huge = HugeSize;
  void *MallocFail = malloc(Huge);
  void *MmapFail =
      mmap(0, Huge, PROT_READ | PROT_WRITE,
           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  void *MallocOK = malloc(16);
  int Failed = 0;

  CHECK_TRUE("malloc == NULL", MallocFail == NULL);
  CHECK_TRUE("NULL == malloc", NULL == MallocFail);
  CHECK_TRUE("!malloc", !MallocFail);
  CHECK_TRUE("malloc == get_zero", MallocFail == get_zero());
  CHECK_FALSE("malloc == get_one", MallocFail == get_one());
  CHECK_FALSE("malloc || false", MallocFail || get_false_pointer());
  CHECK_FALSE("malloc && one", MallocFail && get_one());

  CHECK_TRUE("mmap == MAP_FAILED", MmapFail == MAP_FAILED);
  CHECK_TRUE("MAP_FAILED == mmap", MAP_FAILED == MmapFail);
  CHECK_TRUE("mmap == get_minus_one", MmapFail == get_minus_one());
  CHECK_FALSE("mmap == get_one", MmapFail == get_one());

  CHECK_FALSE("malloc_ok == NULL", MallocOK == NULL);
  CHECK_FALSE("malloc_ok == get_zero", MallocOK == get_zero());
  CHECK_FALSE("malloc_ok == get_one", MallocOK == get_one());
  CHECK_FALSE("malloc_ok == get_minus_one", MallocOK == get_minus_one());

  free(MallocOK);

  if (Failed) {
    return 1;
  }

  puts("ok");
  return 0;
}

// CHECK: ok
// CHECK-NOT: FAIL
