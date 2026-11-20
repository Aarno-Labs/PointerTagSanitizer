//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Exercise nested input ranges through musl's public writev API.
// RUN: %ptsan_musl_strict -o %t %s
// RUN: %run %t | FileCheck %s

// CHECK: nested write works

#include <sys/uio.h>
#include <unistd.h>

int main(void) {
  char first[] = "nested ";
  char second[] = "write works\n";
  struct iovec vectors[] = {
      {first, sizeof(first) - 1},
      {second, sizeof(second) - 1},
  };

  ssize_t expected = sizeof(first) + sizeof(second) - 2;
  return writev(STDOUT_FILENO, vectors, 2) == expected ? 0 : 1;
}
