//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// A byte GEP from null can carry an entire encoded pointer in its index.
// Perl's floating-point formatting loop acquired this form through SCEV
// expansion. Treating it as stripped makes an equal-address comparison fail.
// Supply ordinary LLVM IR to retain the form until PTSan preparation runs;
// Clang can otherwise simplify a C integer-to-pointer round-trip beforehand.
// RUN: split-file %s %t.dir
// RUN: %opt_ptsan -mtriple=%ptsan_target_triple %t.dir/roundtrip.ll -o %t.id16.ll
// RUN: %clang_raw -c %t.id16.ll -o %t.id16.o
// RUN: %clang_ptsan -O2 %t.dir/main.c %t.id16.o -o %t.id16
// RUN: %run %t.id16
// RUN: %opt_ptsan -mtriple=%ptsan_target_triple -ptsan-32-bit-ids %t.dir/roundtrip.ll -o %t.id32.ll
// RUN: %clang_raw -c %t.id32.ll -o %t.id32.o
// RUN: %clang_ptsan_id32 -O2 %t.dir/main.c %t.id32.o -o %t.id32
// RUN: %run %t.id32

//--- roundtrip.ll
target datalayout = "e-p:64:64"

define i32 @roundtrip_equal(ptr %end) {
  %bits = ptrtoint ptr %end to i64
  %reconstructed = getelementptr i8, ptr null, i64 %bits
  %equal = icmp eq ptr %end, %reconstructed
  %result = zext i1 %equal to i32
  ret i32 %result
}

//--- main.c
#include <assert.h>
#include <stdlib.h>

int roundtrip_equal(void *end);

int main(void) {
  char *p = malloc(17);
  assert(p);
  assert(roundtrip_equal(p + 1));
  free(p);
  return 0;
}
