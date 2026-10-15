//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// AArch64 passes a caller-allocated copy whose ID remains live until the
// caller returns; PTSan does not yet retire its bounds at llvm.lifetime.end.
// x86-64's byval copy gets a callee-owned ID, retired when escape_copy returns.
// XFAIL: aarch64-target-arch
//
// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe 2> err.txt
// RUN: FileCheck %s < err.txt

// CHECK: Found bad POINT access

#include <stdio.h>

typedef struct {
  char payload[2048];
} large_blob_t;

static char *escaped;
static volatile char sink;

__attribute__((noinline))
static void escape_copy(large_blob_t blob) {
  blob.payload[0] = 'x';
  escaped = blob.payload;
}

__attribute__((noinline))
static void use_escaped_copy(void) {
  sink = escaped[0];
}

int main(void) {
  large_blob_t blob = {0};
  escape_copy(blob);
  use_escaped_copy();
  fprintf(stderr, "sink=%c\n", sink);
  return 0;
}
