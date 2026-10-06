//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Musl's vDSO fast paths are uninstrumented and cannot write through tagged
// output pointers. PTSan's musl build routes these APIs through syscalls.
//
// RUN: %ptsan_musl_strict -o %t %s
// RUN: %run %t

#define _GNU_SOURCE
#include <sched.h>
#include <sys/time.h>
#include <time.h>

int main(void) {
  struct timespec timestamp;
  if (clock_gettime(CLOCK_REALTIME, &timestamp) != 0 || timestamp.tv_nsec < 0 ||
      timestamp.tv_nsec >= 1000000000) {
    return 1;
  }

  struct timeval time_value;
  if (gettimeofday(&time_value, NULL) != 0 || time_value.tv_usec < 0 ||
      time_value.tv_usec >= 1000000) {
    return 2;
  }

  time_t stored_time;
  time_t returned_time = time(&stored_time);
  if (returned_time == (time_t)-1 || stored_time != returned_time) {
    return 3;
  }

  if (sched_getcpu() < 0) {
    return 4;
  }
  return 0;
}
