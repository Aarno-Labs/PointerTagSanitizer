//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict -Wall -Wextra -Werror %s -o %t
// RUN: %run %t
// RUN: not %run %t epoll 2>&1 | FileCheck %s
// RUN: not %run %t pselect 2>&1 | FileCheck %s
// RUN: not %run %t wait 2>&1 | FileCheck %s
// RUN: not %run %t suspend 2>&1 | FileCheck %s
// RUN: not %run %t poll 2>&1 | FileCheck %s
// RUN: not %run %t pending 2>&1 | FileCheck %s
// RUN: not %run %t signalfd4 2>&1 | FileCheck %s
// RUN: %ptsan_musl_id32_strict -Wall -Wextra -Werror %s -o %t.id32
// RUN: %run %t.id32
// RUN: not %run %t.id32 epoll 2>&1 | FileCheck %s
// RUN: not %run %t.id32 pselect 2>&1 | FileCheck %s
// RUN: not %run %t.id32 wait 2>&1 | FileCheck %s
// RUN: not %run %t.id32 suspend 2>&1 | FileCheck %s
// RUN: not %run %t.id32 poll 2>&1 | FileCheck %s
// RUN: not %run %t.id32 pending 2>&1 | FileCheck %s
// RUN: not %run %t.id32 signalfd4 2>&1 | FileCheck %s
// CHECK: Found bad libc access

// The raw x86-64/AArch64 Linux ABI uses eight bytes, not libc's sigset_t.
// Exact-sized heap buffers must work; seven-byte buffers must be rejected.
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t handled;
static void handler(int signo) { handled = signo; }

static void test(const char *operation, int bad) {
  const size_t mask_size = sizeof(uint64_t);
  void *mask = calloc(1, mask_size - bad);
  assert(mask);
  struct timespec timeout = {0, 0};
  if (!strcmp(operation, "epoll")) {
    int fd = epoll_create1(EPOLL_CLOEXEC);
    assert(fd >= 0);
    struct epoll_event event;
    assert(syscall(SYS_epoll_pwait, fd, &event, 1, 0, mask, mask_size) == 0);
    assert(close(fd) == 0);
  } else if (!strcmp(operation, "pselect")) {
    struct {
      void *mask;
      size_t size;
    } descriptor = {mask, mask_size};
    assert(syscall(SYS_pselect6, 0, NULL, NULL, NULL, &timeout, &descriptor) == 0);
  } else if (!strcmp(operation, "wait")) {
    assert(syscall(SYS_rt_sigtimedwait, mask, NULL, &timeout, mask_size) == -1);
    assert(errno == EAGAIN);
  } else if (!strcmp(operation, "suspend")) {
    // Queue a blocked signal first so suspension completes without a timer.
    struct sigaction action = {0}, old_action;
    action.sa_handler = handler;
    assert(sigemptyset(&action.sa_mask) == 0);
    assert(sigaction(SIGUSR1, &action, &old_action) == 0);
    sigset_t blocked, old_mask;
    assert(sigemptyset(&blocked) == 0);
    assert(sigaddset(&blocked, SIGUSR1) == 0);
    assert(sigprocmask(SIG_BLOCK, &blocked, &old_mask) == 0);
    handled = 0;
    assert(raise(SIGUSR1) == 0);
    assert(syscall(SYS_rt_sigsuspend, mask, mask_size) == -1);
    assert(errno == EINTR && handled == SIGUSR1);
    assert(sigprocmask(SIG_SETMASK, &old_mask, NULL) == 0);
    assert(sigaction(SIGUSR1, &old_action, NULL) == 0);
  } else if (!strcmp(operation, "poll")) {
    struct pollfd fd = {.fd = -1};
    assert(syscall(SYS_ppoll, &fd, 1, &timeout, mask, mask_size) == 0);
  } else if (!strcmp(operation, "pending")) {
    assert(syscall(SYS_rt_sigpending, mask, mask_size) == 0);
  } else if (!strcmp(operation, "signalfd4")) {
    int fd = syscall(SYS_signalfd4, -1, mask, mask_size, SFD_CLOEXEC);
    assert(fd >= 0);
    assert(close(fd) == 0);
#ifdef SYS_signalfd
  } else if (!strcmp(operation, "signalfd")) {
    int fd = syscall(SYS_signalfd, -1, mask, mask_size);
    assert(fd >= 0);
    assert(close(fd) == 0);
#endif
  } else {
    assert(0 && "unknown operation");
  }
  assert(!bad && "expected a bounds failure for the undersized signal set");
  free(mask);
}

int main(int argc, char **argv) {
  if (argc == 2) {
    test(argv[1], 1);
  } else {
    assert(argc == 1);
    const char *operations[] = {
        "epoll", "pselect", "wait", "suspend", "poll", "pending", "signalfd4",
#ifdef SYS_signalfd
        "signalfd",
#endif
    };
    for (unsigned i = 0; i < sizeof(operations) / sizeof(operations[0]); ++i)
      test(operations[i], 0);
  }
  return 0;
}
