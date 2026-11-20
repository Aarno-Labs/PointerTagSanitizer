//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict -o %t %s
// RUN: %run %t

// Nested ID32 destination pointers must reach the kernel as raw addresses.
// RUN: %ptsan_musl_id32_nonstrict %s -o %t.id32
// RUN: %run %t.id32
// RUN: %ptsan_musl_id32_strict %s -o %t.id32.strict
// RUN: %run %t.id32.strict

#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

int main(void) {
  int fds[2];
  if (pipe(fds) != 0)
    return 1;

  char source[] = "read works";
  if (write(fds[1], source, sizeof(source) - 1) != sizeof(source) - 1)
    return 2;

  char first[4] = {0};
  char second[7] = {0};
  struct iovec vectors[] = {
      {first, sizeof(first)},
      {second, sizeof(second) - 1},
  };
  if (readv(fds[0], vectors, 2) != sizeof(source) - 1)
    return 3;

  char result[sizeof(source)] = {0};
  memcpy(result, first, sizeof(first));
  memcpy(result + sizeof(first), second, sizeof(second) - 1);
  return memcmp(result, source, sizeof(source)) == 0 ? 0 : 4;
}
