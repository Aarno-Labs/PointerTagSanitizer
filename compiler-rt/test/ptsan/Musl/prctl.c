//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Exercise representative scalar, input-buffer, and output-buffer prctl
// operations through the public musl API.
//
// RUN: %ptsan_musl_strict -Wall -Wextra -Werror -o %t %s
// RUN: %run %t good
// RUN: not %run %t bad-name 2> %t.name
// RUN: FileCheck %s < %t.name
// RUN: not %run %t bad-signal 2> %t.signal
// RUN: FileCheck %s < %t.signal
//
// CHECK: Found bad

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>

static int testGood(void) {
  char original_name[16];
  if (prctl(PR_GET_NAME, (unsigned long)original_name, 0UL, 0UL, 0UL) != 0) {
    return 1;
  }

  char new_name[] = "ptsan-prctl";
  if (prctl(PR_SET_NAME, (unsigned long)new_name, 0UL, 0UL, 0UL) != 0) {
    return 2;
  }

  char returned_name[16];
  if (prctl(PR_GET_NAME, (unsigned long)returned_name, 0UL, 0UL, 0UL) != 0 ||
      strcmp(returned_name, new_name) != 0) {
    return 3;
  }

  if (prctl(PR_SET_NAME, (unsigned long)original_name, 0UL, 0UL, 0UL) != 0) {
    return 4;
  }

  int parent_signal = -1;
  if (prctl(PR_GET_PDEATHSIG, (unsigned long)&parent_signal, 0UL, 0UL, 0UL) !=
      0) {
    return 5;
  }

  if (prctl(PR_GET_DUMPABLE, 0UL, 0UL, 0UL, 0UL) < 0) {
    return 6;
  }
  return 0;
}

static int testBadName(void) {
  char *name = malloc(15);
  if (!name) {
    return 1;
  }
  (void)prctl(PR_GET_NAME, (unsigned long)name, 0UL, 0UL, 0UL);
  free(name);
  return 0;
}

static int testBadSignal(void) {
  char *signal = malloc(sizeof(int) - 1);
  if (!signal) {
    return 1;
  }
  (void)prctl(PR_GET_PDEATHSIG, (unsigned long)signal, 0UL, 0UL, 0UL);
  free(signal);
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    return 1;
  }
  if (!strcmp(argv[1], "good")) {
    return testGood();
  }
  if (!strcmp(argv[1], "bad-name")) {
    return testBadName();
  }
  if (!strcmp(argv[1], "bad-signal")) {
    return testBadSignal();
  }
  return 2;
}
