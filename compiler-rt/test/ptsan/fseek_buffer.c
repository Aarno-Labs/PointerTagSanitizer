//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %clang_ptsan -O2 -mllvm -ptsan-abort-on-error -o %t %s
// RUN: %t %t.data 2>&1 | FileCheck %s
// RUN: %t %t.data fseek 2>&1 | FileCheck %s
// RUN: %clang_ptsan -O2 -D_FILE_OFFSET_BITS=64 -S -emit-llvm -o - %s \
// RUN:   | FileCheck %s --check-prefix=LARGEFILE
// RUN: %clang_ptsan -O2 -D_FILE_OFFSET_BITS=64 -mllvm -ptsan-abort-on-error \
// RUN:   -o %t.64 %s
// RUN: %t.64 %t.data 2>&1 | FileCheck %s

// Seeking a freshly opened file can allocate libc's stream buffer before any
// fread/fwrite interceptor runs. fseek, fseeko, and fseeko64 (selected through
// _FILE_OFFSET_BITS=64) must keep that internal allocation untagged.
// A real file (not fmemopen) makes fread refill the buffer
// through the kernel; an accidentally tagged buffer fails with EFAULT.

// LARGEFILE: call i32 @fseeko64(
// CHECK-NOT: Found bad
// CHECK: fseek-buffer-ok
// CHECK-NOT: Found bad

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
  assert(argc == 2 || argc == 3);
  const size_t size = 65578;
  char *data = malloc(size);
  assert(data);
  memset(data, 42, size);

  FILE *stream = fopen(argv[1], "wb");
  assert(stream);
  assert(fwrite(data, 1, size, stream) == size);
  assert(fclose(stream) == 0);

  stream = fopen(argv[1], "rb");
  assert(stream);
  if (argc == 3) {
    assert(fseek(stream, -65578, SEEK_END) == 0);
  } else {
    assert(fseeko(stream, -65578, SEEK_END) == 0);
  }
  memset(data, 0, size);
  assert(fread(data, 1, size, stream) == size);
  assert(!ferror(stream));
  for (size_t i = 0; i < size; ++i) {
    assert(data[i] == 42);
  }

  assert(fclose(stream) == 0);
  free(data);
  puts("fseek-buffer-ok");
  return 0;
}
