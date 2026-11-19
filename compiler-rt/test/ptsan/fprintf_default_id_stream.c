//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %clang_ptsan -O2 %s -o %t
// RUN: %run %t

#include "Inputs/ptsan_test_interface.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

__attribute__((disable_sanitizer_instrumentation, noinline))
static FILE *with_default_id(FILE *stream) {
  uintptr_t raw = (uintptr_t)stream & UINT64_C(0x0000ffffffffffff);
  return (FILE *)(raw | (uintptr_t)ptsan_default_id() << 48);
}

int main(void) {
  FILE *stream = tmpfile();
  assert(stream);
  assert(fprintf(with_default_id(stream), "version %d\n", 2) == 10);

  rewind(stream);
  char contents[16] = {0};
  assert(fread(contents, 1, sizeof(contents) - 1, stream) == 10);
  assert(strcmp(contents, "version 2\n") == 0);
  assert(fclose(stream) == 0);
  return 0;
}
