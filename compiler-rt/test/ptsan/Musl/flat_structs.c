//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Exercise public APIs with fixed-size output and optional input/output
// structures.  A zero-valued timer leaves ITIMER_REAL disabled.
//
// RUN: %ptsan_musl_strict -o %t %s
// RUN: %run %t good
// RUN: not %run %t bad-sysinfo 2> %t.sysinfo
// RUN: FileCheck %s < %t.sysinfo
// RUN: not %run %t bad-timer-input 2> %t.timer-input
// RUN: FileCheck %s < %t.timer-input
// RUN: not %run %t bad-timer-output 2> %t.timer-output
// RUN: FileCheck %s < %t.timer-output
//
// CHECK: Found bad

#include <stdlib.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <sys/time.h>

static int test_good(void) {
  struct sysinfo *info = malloc(sizeof(*info));
  struct itimerval *new_timer = calloc(1, sizeof(*new_timer));
  struct itimerval *old_timer = malloc(sizeof(*old_timer));
  if (!info || !new_timer || !old_timer) {
    free(old_timer);
    free(new_timer);
    free(info);
    return 1;
  }

  int result = 0;
  if (sysinfo(info) != 0)
    result = 2;
  else if (setitimer(ITIMER_REAL, new_timer, old_timer) != 0)
    result = 3;

  free(old_timer);
  free(new_timer);
  free(info);
  return result;
}

static int test_bad_sysinfo(void) {
  // Validate the public API object size rather than encoding a kernel ABI
  // prefix size in the interceptor.
  char *buffer = malloc(sizeof(struct sysinfo) - 1);
  if (!buffer)
    return 1;
  (void)sysinfo((struct sysinfo *)buffer);
  free(buffer);
  return 0;
}

static int test_bad_timer_input(void) {
  char *buffer = calloc(1, sizeof(struct itimerval) - 1);
  if (!buffer)
    return 1;
  (void)setitimer(ITIMER_REAL, (const struct itimerval *)buffer, 0);
  free(buffer);
  return 0;
}

static int test_bad_timer_output(void) {
  struct itimerval new_timer = {{0, 0}, {0, 0}};
  char *buffer = malloc(sizeof(struct itimerval) - 1);
  if (!buffer)
    return 1;
  (void)setitimer(ITIMER_REAL, &new_timer, (struct itimerval *)buffer);
  free(buffer);
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 2)
    return 1;
  if (!strcmp(argv[1], "good"))
    return test_good();
  if (!strcmp(argv[1], "bad-sysinfo"))
    return test_bad_sysinfo();
  if (!strcmp(argv[1], "bad-timer-input"))
    return test_bad_timer_input();
  if (!strcmp(argv[1], "bad-timer-output"))
    return test_bad_timer_output();
  return 2;
}
