//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Exercise cancellation while a thread is inside musl's cancellation-point
// syscall path. The timed join keeps a regression deterministic.
// Strict cancellation hits the deferred kernel-context handling documented
// in signal_context_strict_xfail.c; this test exercises supported non-strict
// cancellation with non-strict musl.
//
// RUN: %ptsan_musl_nonstrict -o %t %s
// RUN: %run %t

#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

static int fds[2];
static pthread_mutex_t entered_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t entered_condition = PTHREAD_COND_INITIALIZER;
static int entered;

static void *blocking_read(void *unused) {
  (void)unused;
  char byte;
  pthread_mutex_lock(&entered_mutex);
  entered = 1;
  pthread_cond_signal(&entered_condition);
  pthread_mutex_unlock(&entered_mutex);
  (void)read(fds[0], &byte, sizeof(byte));
  return 0;
}

int main(void) {
  if (pipe(fds) != 0) {
    return 1;
  }

  pthread_t thread;
  if (pthread_create(&thread, 0, blocking_read, 0) != 0) {
    return 2;
  }

  pthread_mutex_lock(&entered_mutex);
  while (!entered) {
    pthread_cond_wait(&entered_condition, &entered_mutex);
  }
  pthread_mutex_unlock(&entered_mutex);

  if (pthread_cancel(thread) != 0) {
    return 3;
  }

  struct timespec deadline;
  if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
    return 4;
  }
  deadline.tv_sec += 2;

  void *result = 0;
  int join_result = pthread_timedjoin_np(thread, &result, &deadline);
  if (join_result == ETIMEDOUT) {
    char byte = 0;
    if (write(fds[1], &byte, sizeof(byte)) != sizeof(byte)) {
      return 5;
    }
    if (pthread_join(thread, &result) != 0) {
      return 6;
    }
    return 7;
  }
  if (join_result != 0) {
    return 8;
  }
  return result == PTHREAD_CANCELED ? 0 : 9;
}
