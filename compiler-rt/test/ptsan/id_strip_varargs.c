//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Strip-only variadic callees must work without allocating IDs or checking
// va_list accesses. Calls back into instrumented variadic code still need the
// sentinel that supplies argument bounds. Exercise register and stack arguments,
// and require the instrumented callee to diagnose an extra va_arg.
// REQUIRES: x86_64-target-arch
// RUN: split-file %s %t.dir
// RUN: %clang_strip -O2 -c %t.dir/library.c -o %t.library.o
// RUN: %clang_strip -O2 -fsanitize-ptsan-runtime-validate -S -emit-llvm %t.dir/library.c -o %t.library.ll
// RUN: FileCheck %s --check-prefix=LIBRARY --implicit-check-not='@ptsan_get_stack_ids' --implicit-check-not='@ptsan_init_va_list' --implicit-check-not='@ptsan_complain' --implicit-check-not='@ptsan_validate_function_start' --implicit-check-not='@ClStrictMode' < %t.library.ll
// RUN: %clang_ptsan -O2 -mllvm -ptsan-abort-on-error %t.dir/main.c %t.library.o -o %t
// RUN: %run %t
// RUN: not %run %t overread > %t.out 2>&1
// RUN: FileCheck %s --check-prefix=OVERREAD < %t.out

// Shared strip-only libraries resolve the sentinel from the executable. Match
// the dependency build's override of LLVM's default -z defs linker flag.
// RUN: %clang_strip -O2 -fPIC -shared -Wl,-z,defs -Wl,-z,undefs %t.dir/library.c -o %t.dir/library.so
// RUN: %clang_ptsan -O2 -mllvm -ptsan-abort-on-error %t.dir/main.c %t.dir/library.so -Wl,-rpath,%t.dir -o %t
// RUN: %run %t
// RUN: not %run %t overread > %t.out 2>&1
// RUN: FileCheck %s --check-prefix=OVERREAD < %t.out

// RUN: %clang_strip -fsanitize-ptsan-32-bit-ids -DPTSAN_TEST_32_BIT_IDS -O2 -c %t.dir/library.c -o %t.library.o
// RUN: %clang_strip -fsanitize-ptsan-32-bit-ids -DPTSAN_TEST_32_BIT_IDS -O2 -fsanitize-ptsan-runtime-validate -S -emit-llvm %t.dir/library.c -o %t.library.ll
// RUN: FileCheck %s --check-prefix=LIBRARY --implicit-check-not='@ptsan_get_stack_ids' --implicit-check-not='@ptsan_init_va_list' --implicit-check-not='@ptsan_complain' --implicit-check-not='@ptsan_validate_function_start' --implicit-check-not='@ClStrictMode' < %t.library.ll
// RUN: %clang_ptsan_id32 -O2 -mllvm -ptsan-abort-on-error %t.dir/main.c %t.library.o -o %t
// RUN: %run %t
// RUN: not %run %t overread > %t.out 2>&1
// RUN: FileCheck %s --check-prefix=OVERREAD < %t.out
// RUN: %clang_strip -fsanitize-ptsan-32-bit-ids -O2 -fPIC -shared -Wl,-z,defs -Wl,-z,undefs %t.dir/library.c -o %t.dir/library.so
// RUN: %clang_ptsan_id32 -O2 -mllvm -ptsan-abort-on-error %t.dir/main.c %t.dir/library.so -Wl,-rpath,%t.dir -o %t
// RUN: %run %t
// RUN: not %run %t overread > %t.out 2>&1
// RUN: FileCheck %s --check-prefix=OVERREAD < %t.out
//
// LIBRARY: @__ptsan_va_sentinel = external
// LIBRARY: call {{.*}}byval([2 x i64]){{.*}}@__ptsan_va_sentinel
// OVERREAD: Found bad {{.*}} access

//--- library.c
#include <stdarg.h>

long invoke(long (*reader)(int, ...), int count) {
  return reader(count, 1L, 2L, 3L, 4L, 5L, 6L, 7L, 8L);
}

// Strip-only va_start must remain uninstrumented: the library owns no IDs.
long stripped_sum(int count, ...) {
  va_list args;
  va_start(args, count);
  long sum = 0;
  for (int i = 0; i < count; ++i) {
    sum += va_arg(args, long);
  }
  va_end(args);
  return sum;
}

//--- main.c
#include <assert.h>
#include <stdarg.h>

long invoke(long (*reader)(int, ...), int count);
long stripped_sum(int count, ...);

__attribute__((noinline)) long instrumented_sum(int count, ...) {
  va_list args;
  va_start(args, count);
  long sum = 0;
  for (int i = 0; i < count; ++i) {
    sum += va_arg(args, long);
  }
  va_end(args);
  return sum;
}

int main(int argc, char **argv) {
  assert(stripped_sum(3, 4L, 5L, 6L) == 15);
  // Five variadic longs fit in registers; the remaining three are on-stack.
  // Reading a ninth must fail at this call's marker, not an outer marker.
  assert(invoke(instrumented_sum, argc == 1 ? 8 : 9) == 36);
  return 0;
}
