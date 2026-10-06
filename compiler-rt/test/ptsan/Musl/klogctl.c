//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Exercise public musl klogctl actions with accessed and ignored buffers.
//
// RUN: %ptsan_musl_strict -o %t %s
// RUN: %run %t good
// RUN: not %run %t bad 2> %t.bad
// RUN: FileCheck %s < %t.bad
//
// CHECK: Found bad

#include <stdlib.h>
#include <string.h>
#include <sys/klog.h>

enum {
  KernelLogRead = 2,
  KernelLogReadAll = 3,
  KernelLogReadClear = 4,
  KernelLogSizeBuffer = 10,
};

static int test_good(void) {
  char buffer[16];
  memset(buffer, 0, sizeof(buffer));

  (void)klogctl(KernelLogRead, buffer, sizeof(buffer));
  (void)klogctl(KernelLogReadAll, buffer, sizeof(buffer));
  (void)klogctl(KernelLogReadClear, buffer, sizeof(buffer));

  char *ignored_buffer = buffer + sizeof(buffer);
  (void)klogctl(KernelLogSizeBuffer, ignored_buffer, 0);
  (void)klogctl(KernelLogReadAll, ignored_buffer, -1);
  return 0;
}

static int test_bad(void) {
  const int requested_size = 16;
  void *buffer = calloc(1, requested_size - 1);
  if (!buffer) {
    return 1;
  }
  (void)klogctl(KernelLogReadAll, (char *)buffer, requested_size);
  free(buffer);
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    return 1;
  }
  if (!strcmp(argv[1], "good")) {
    return test_good();
  }
  if (!strcmp(argv[1], "bad")) {
    return test_bad();
  }
  return 2;
}
