//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Check a non-default quantum in both compiler-generated stack metadata and
// runtime allocation metadata. Raw inspection prevents alignment folding.
// RUN: split-file %s %t.dir
// RUN: %clang_raw -O2 -DPTSAN_TEST_32_BIT_IDS -DTEST_QUANTUM=65536 -I%S -c %t.dir/inspect.c -o %t.inspect.o
// RUN: %clang_ptsan_id32 -O2 -mllvm -ptsan-32-bit-ids-origin-quantum=65536 -mllvm -ptsan-random-seed=1 %t.dir/main.c %t.inspect.o -o %t
// RUN: %run %t
// RUN: %clang_ptsan_id32 -O2 -mllvm -ptsan-32-bit-ids-origin-quantum=65536 -mllvm -ptsan-stack-temporal-safety=0 %t.dir/main.c %t.inspect.o -o %t.weak
// RUN: %run %t.weak
// RUN: %clang_ptsan_id32 -O2 -mllvm -ptsan-32-bit-ids-origin-quantum=65536 -mllvm -ptsan-stack-temporal-safety=0 -mllvm -ptsan-stop-after=lowering -S -emit-llvm %t.dir/main.c -o - | FileCheck %s --check-prefix=IR --implicit-check-not=@llvm.umax
// RUN: not %clang_ptsan_id32 -mllvm -ptsan-32-bit-ids-origin-quantum=3 -c %t.dir/main.c -o %t.bad.o 2>&1 | FileCheck %s --check-prefix=OPTION
// RUN: %clang_ptsan_id32 -O2 -mllvm -ptsan-32-bit-ids-origin-quantum=1024 %t.dir/main.c %t.inspect.o -o %t.small
// RUN: not %run %t.small 2>&1 | FileCheck %s --check-prefix=STARTUP
//
// The masks must be constants, including when requested alignment is larger.
// IR-LABEL: define {{.*}}@stack_probe(
// IR-DAG: and i64 {{.*}}, 65535
// IR-DAG: and i64 {{.*}}, 131071
// IR: ret void
// OPTION: PTSan ID32 origin quantum must be a power of two no larger than 2 GiB
// STARTUP: PTSan ID32 origin quantum (1024) is smaller than the kernel page size
//
//--- inspect.c
#include "Inputs/ptsan_test_interface.h"
#include <assert.h>
#include <stdint.h>

extern unsigned ClID32OriginQuantum;

void inspect(const void *pointer, unsigned alignment) {
  uint64_t quantum = alignment > TEST_QUANTUM ? alignment : TEST_QUANTUM;
  uint64_t raw = ptsan_get_address(pointer);
  assert(ClID32OriginQuantum == TEST_QUANTUM);
  assert(get_id(pointer) != 0);
  assert((uintptr_t)pointer % quantum == raw % quantum);
}

//--- main.c
#include <assert.h>
#include <stdlib.h>

void inspect(const void *, unsigned);
static volatile char Global[32];

__attribute__((noinline)) void stack_probe(void) {
  _Alignas(16) volatile char ordinary[32];
  _Alignas(131072) volatile char aligned[32];
  ordinary[0] = 17;
  aligned[0] = 34;
  inspect((const void *)ordinary, 16);
  inspect((const void *)aligned, 131072);
  assert(ordinary[0] == 17 && aligned[0] == 34);
}

int main(void) {
  Global[0] = 7;
  inspect((const void *)Global, 1);
  stack_probe();
  void *heap = malloc(64);
  assert(heap != NULL);
  inspect(heap, 16);
  free(heap);
  void *aligned = NULL;
  assert(posix_memalign(&aligned, 131072, 64) == 0);
  inspect(aligned, 131072);
  free(aligned);
  return 0;
}
