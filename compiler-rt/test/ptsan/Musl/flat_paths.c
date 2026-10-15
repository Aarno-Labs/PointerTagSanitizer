//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Public pathname APIs exercise NUL-terminated input strings, while readlink
// also supplies a separately size-counted output buffer.
//
// RUN: %ptsan_musl_strict -o %t %s
// RUN: %run %t good
// RUN: not %run %t bad-path 2> %t.path
// RUN: FileCheck %s < %t.path
// RUN: not %run %t bad-output 2> %t.output
// RUN: FileCheck %s < %t.output
//
// CHECK: Found bad

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char ProcSelfExe[] = "/proc/self/exe";

static int test_good(void) {
  char *path = malloc(sizeof(ProcSelfExe));
  char *output = malloc(256);
  if (!path || !output) {
    free(output);
    free(path);
    return 1;
  }
  memcpy(path, ProcSelfExe, sizeof(ProcSelfExe));

  int result = 0;
  if (access(path, F_OK) != 0)
    result = 2;
  else if (readlink(path, output, 256) <= 0)
    result = 3;

  free(output);
  free(path);
  return result;
}

static int test_bad_path(void) {
  char *path = malloc(1);
  if (!path)
    return 1;
  path[0] = '/';
  // The string starts one byte past the allocation, so its first byte is
  // deterministically outside the pointer's range.
  (void)access(path + 1, F_OK);
  free(path);
  return 0;
}

static int test_bad_output(void) {
  char *output = malloc(4);
  if (!output)
    return 1;
  (void)readlink(ProcSelfExe, output, 256);
  free(output);
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 2)
    return 1;
  if (!strcmp(argv[1], "good"))
    return test_good();
  if (!strcmp(argv[1], "bad-path"))
    return test_bad_path();
  if (!strcmp(argv[1], "bad-output"))
    return test_bad_output();
  return 2;
}
