//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Origins must preserve requested alignment for globals, TLS, fixed and
// variable stack objects, and aligned heap allocations. Resizing must retain
// the decode origin without allowing accesses outside the updated bounds.

// RUN: split-file %s %t.dir
// RUN: %clang_raw -O2 -DPTSAN_TEST_32_BIT_IDS -I%S -c %t.dir/inspect.c -o %t.inspect.o
// RUN: %clang_ptsan_id32 -O2 -mllvm -ptsan-random-seed=1 %t.dir/main.c %t.inspect.o -o %t
// RUN: %run %t
// RUN: %clang_ptsan_id32 -O2 -mllvm -ptsan-stack-temporal-safety=0 %t.dir/main.c %t.inspect.o -o %t.weak
// RUN: %run %t.weak
// RUN: not %run %t impossible 2>&1 | FileCheck %s
// CHECK: CHECK failed: {{.*}}Alignment

// Inspect the representation out of line without instrumentation, so alignment
// attributes cannot fold away the assertions on encoded pointer bits.
//--- inspect.c
#include "Inputs/ptsan_test_interface.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>

extern uint32_t ptsan_get_next_global_id(void *, uint64_t, uint64_t);
extern uint32_t ptsan_choose_stack_origin(uint64_t);
extern void ptsan_resize_buffer(uint32_t, uint64_t);
extern bool ptsan_check(void *, uint64_t);

void inspect(const void *p, unsigned alignment) {
  uint64_t raw = ptsan_get_address(p);
  assert(ptsan_get_id(p) != 0);
  assert((raw & (alignment - 1)) == 0);
  assert(((uintptr_t)p & (alignment - 1)) == 0);
}

void check_global_origins(void) {
  // Global registration randomizes even with stack temporal safety disabled,
  // and retains the raw page offset independently of object size.
  uint64_t page_size = (uint64_t)sysconf(_SC_PAGESIZE);
  void *base = (void *)UINT64_C(0x12341008);
  uint32_t first_origin = 0;
  bool varied = false;
  for (unsigned i = 0; i < 32; ++i) {
    PTSanID id = ptsan_get_next_global_id(base, i % 2 ? 32 : 8192, 1);
    uint32_t origin = ptsan_get_origin(id);
    assert(origin < UINT32_C(0x80000000));
    assert(origin % page_size == (uintptr_t)base % page_size);
    if (i == 0) {
      first_origin = origin;
    } else {
      varied |= origin != first_origin;
    }
  }
  assert(varied);

  // Randomization also preserves stronger requested alignment.
  PTSanID aligned = ptsan_get_next_global_id(
      (void *)UINT64_C(0x100000000), 32, 65536);
  assert(ptsan_get_origin(aligned) % 65536 == 0);
}

void check_resize(void) {
  // Synthetic registration tests metadata updates without depending on the
  // system allocator deciding to extend a particular allocation in place.
  const uint64_t raw = 0x12341000;
  uint32_t id = ptsan_get_next_global_id((void *)raw, 32, 4096);
  uint64_t origin = ptsan_get_origin(id);
  uint64_t page_size = (uint64_t)sysconf(_SC_PAGESIZE);
  assert(origin % page_size == raw % page_size);
  uint64_t tag = (uint64_t)id << 32;
  void *p = (void *)(tag | origin);
  ptsan_resize_buffer(id, 8192);
  assert(ptsan_get_id(p) == id);
  assert(ptsan_get_origin(id) == origin);
  assert(ptsan_get_size(id) == 8192);
  assert(ptsan_get_address(p) == raw);

  // Even the largest supported span preserves the pointer and its alignment.
  ptsan_resize_buffer(id, UINT64_C(0x7fffffff));
  assert(ptsan_get_origin(id) == origin);
  assert(ptsan_get_size(id) == UINT64_C(0x7fffffff));
  assert(ptsan_get_address(p) == raw);
  inspect(p, 4096);

  // Bounds checks must reject unsigned offset underflow, including when the
  // object has the largest supported span.
  assert(ptsan_check(p, 1));
  assert(!ptsan_check((void *)(tag | (origin - 1)), 1));
  assert(ptsan_check((void *)(tag | (origin + 0x7ffffffe)), 1));
  assert(!ptsan_check((void *)(tag | (origin + 0x7fffffff)), 1));
}

void impossible_alignment(void) {
  // At 2 GiB alignment no nonzero salt fits in the allowed range.
  ptsan_choose_stack_origin(UINT64_C(0x80000000));
  assert(0 && "unrepresentable alignment should terminate");
}

//--- main.c
#include <assert.h>
#include <stdlib.h>

void inspect(const void *, unsigned);
void check_global_origins(void);
void check_resize(void);
void impossible_alignment(void);

_Alignas(65536) static const char Constant[128] = { 7 };
_Alignas(4096) static volatile char Global[128];
_Alignas(4096) static _Thread_local volatile char ThreadLocal[128];

__attribute__((noinline)) static void check_stack(unsigned size) {
  _Alignas(64) volatile char fixed[128];
  _Alignas(4096) volatile char dynamic[size];
  fixed[0] = 3;
  dynamic[size - 1] = 4;
  inspect((const void *)fixed, 64);
  inspect((const void *)dynamic, 4096);
  assert(fixed[0] == 3 && dynamic[size - 1] == 4);
}

int main(int argc, char **argv) {
  if (argc > 1) {
    impossible_alignment();
  }
  inspect(Constant, 65536);
  inspect((const void *)Global, 4096);
  inspect((const void *)ThreadLocal, 4096);
  ThreadLocal[0] = 5;
  assert(ThreadLocal[0] == 5);
  Global[127] = Constant[0];
  assert(Global[127] == 7);
  check_stack(128 + (unsigned)argc);
  check_global_origins();
  check_resize();
  for (unsigned i = 0; i < 32; ++i) {
    void *allocation = NULL;
    assert(posix_memalign(&allocation, 64, 128) == 0);
    char *p = __builtin_assume_aligned(allocation, 64);
    assert(p != NULL);
    inspect(p, 64);
    volatile char *v = p;
    v[0] = 1;
    v[127] = 2;
    assert(v[0] == 1 && v[127] == 2);
    free(p);
  }
}
