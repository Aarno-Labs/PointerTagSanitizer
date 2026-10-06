//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %clang_raw -fsanitize=ptsan -O1 -g -fno-discard-value-names -S -emit-llvm -o - %s | FileCheck %s

static const int ScalarConstant = 7;
volatile int Sink;

__attribute__((noinline)) const char *get_literal(void) {
  return "literal without global debug metadata";
}

__attribute__((noinline)) void read_constant_unknown(unsigned index) {
  const unsigned char *bytes = (const unsigned char *)&ScalarConstant;
  Sink = bytes[index];
}

__attribute__((noinline)) int read_constant_known(unsigned index) {
  const volatile unsigned char *bytes =
      (const volatile unsigned char *)&ScalarConstant;
  return bytes[index & 3];
}

// CHECK-DAG: @ScalarConstant.ptsan.ptr = {{.*}}global ptr @ScalarConstant
// CHECK-DAG: @Sink.ptsan.ptr = {{.*}}global ptr @Sink
// CHECK-DAG: @.str.ptsan.ptr = {{.*}}global ptr @.str

// CHECK-LABEL: define {{.*}}@read_constant_unknown(
// CHECK: load ptr, ptr @ScalarConstant.ptsan.ptr
// CHECK: @ptsan_bounds
// CHECK: call void @ptsan_complain
// CHECK: ret void

// CHECK-LABEL: define {{.*}}@read_constant_known(
// CHECK-NOT: @ptsan_bounds
// CHECK-NOT: call void @ptsan_complain
// CHECK: ret i32

// CHECK-LABEL: define internal void @__ptsan_init_tagged_globals(
// CHECK-DAG: call i16 @ptsan_get_constant_global_id(ptr nonnull @ScalarConstant, i64 4)
// CHECK-DAG: call i16 @ptsan_get_constant_global_id(ptr nonnull @.str, i64 38)
// CHECK-DAG: call i16 @ptsan_get_next_global_id(ptr nonnull @Sink, i64 4)
// CHECK: ret void
