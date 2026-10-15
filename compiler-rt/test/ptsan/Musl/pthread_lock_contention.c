//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// A contended musl thread-list lock must wake its waiter through PTSan's
// syscall boundary, which strips the global lock's pointer tag.
//
// RUN: %ptsan_musl_strict -Wall -Wextra -Werror -pthread -o %t %s
// RUN: %run %t

#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>

// Use musl's internal lock helpers to force contention on the tagged global.
extern void __tl_lock(void);
extern void __tl_unlock(void);

static int enter, started;

static void *child(void *arg) {
  while (!__atomic_load_n(&enter, __ATOMIC_ACQUIRE))
    sched_yield();
  __atomic_store_n(&started, 1, __ATOMIC_RELEASE);
  __tl_lock();
  __tl_unlock();
  return arg;
}

int main(void) {
  // A missed futex wake should fail the test rather than hang the suite.
  alarm(10);
  pthread_t thread;
  if (pthread_create(&thread, NULL, child, NULL) != 0)
    return 1;

  __tl_lock();
  __atomic_store_n(&enter, 1, __ATOMIC_RELEASE);
  while (!__atomic_load_n(&started, __ATOMIC_ACQUIRE))
    sched_yield();

  // Give the child time to finish musl's spin phase and enter the futex wait.
  struct timespec delay = {0, 20000000};
  nanosleep(&delay, NULL);
  __tl_unlock();
  return pthread_join(thread, NULL);
}
