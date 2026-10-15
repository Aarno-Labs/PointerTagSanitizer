//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// write is routed through musl's cancellation-point syscall path.  This call
// is not actually cancelled; PTSan validates its flat input range at the
// outgoing syscall boundary.
//
// RUN: %ptsan_musl_strict -o %t %s
// RUN: %run %t good
// RUN: not %run %t bad 2> %t.err
// RUN: FileCheck %s < %t.err

// CHECK: Found bad

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
  if (argc != 2)
    return 1;

  int fds[2];
  if (pipe(fds) != 0)
    return 2;

  char *buffer = malloc(4);
  if (!buffer)
    return 3;
  memcpy(buffer, "data", 4);

  size_t size;
  if (strcmp(argv[1], "good") == 0)
    size = 4;
  else if (strcmp(argv[1], "bad") == 0)
    size = 8;
  else
    return 4;

  (void)write(fds[1], buffer, size);
  free(buffer);
  return 0;
}
