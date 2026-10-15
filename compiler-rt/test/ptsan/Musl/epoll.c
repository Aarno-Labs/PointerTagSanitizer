//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Exercise epoll event arrays and opaque data cookies through public musl APIs.
//
// RUN: %ptsan_musl_strict -o %t %s
// RUN: %run %t good
// RUN: not %run %t bad-control 2> %t.control
// RUN: FileCheck %s < %t.control
// RUN: not %run %t bad-events 2> %t.events
// RUN: FileCheck %s < %t.events
// RUN: not %run %t bad-signal-mask 2> %t.mask
// RUN: FileCheck %s < %t.mask
//
// CHECK: Found bad

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

static int test_good(void) {
  int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
  if (epoll_fd < 0) {
    return 1;
  }

  int pipes[2];
  if (pipe(pipes) != 0) {
    close(epoll_fd);
    return 2;
  }

  struct epoll_event *registration = malloc(sizeof(struct epoll_event));
  struct epoll_event *events = malloc(sizeof(struct epoll_event));
  sigset_t *signal_mask = malloc(sizeof(sigset_t));
  void *cookie = malloc(1);
  if (!registration || !events || !signal_mask || !cookie) {
    free(cookie);
    free(signal_mask);
    free(events);
    free(registration);
    close(pipes[0]);
    close(pipes[1]);
    close(epoll_fd);
    return 3;
  }

  registration->events = EPOLLIN;
  registration->data.ptr = cookie;
  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, pipes[0], registration) != 0) {
    goto fail;
  }
  if (write(pipes[1], "x", 1) != 1) {
    goto fail;
  }

  if (epoll_wait(epoll_fd, events, 1, 0) != 1 || events[0].data.ptr != cookie) {
    goto fail;
  }

  if (sigemptyset(signal_mask) != 0) {
    goto fail;
  }
  if (epoll_pwait(epoll_fd, events, 1, 0, signal_mask) != 1 ||
      events[0].data.ptr != cookie) {
    goto fail;
  }

  struct epoll_event *ignored_event = registration + 1;
  if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, pipes[0], ignored_event) != 0) {
    goto fail;
  }

  errno = 0;
  if (epoll_wait(epoll_fd, ignored_event, 0, 0) != -1 || errno != EINVAL) {
    goto fail;
  }

  errno = 0;
  if (epoll_wait(epoll_fd, ignored_event, -1, 0) != -1 || errno != EINVAL) {
    goto fail;
  }

  errno = 0;
  if (epoll_pwait(epoll_fd, ignored_event, -1, 0, signal_mask) != -1 ||
      errno != EINVAL) {
    goto fail;
  }

  free(cookie);
  free(signal_mask);
  free(events);
  free(registration);
  close(pipes[0]);
  close(pipes[1]);
  close(epoll_fd);
  return 0;

fail:
  free(cookie);
  free(signal_mask);
  free(events);
  free(registration);
  close(pipes[0]);
  close(pipes[1]);
  close(epoll_fd);
  return 4;
}

static int test_bad_control(void) {
  int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
  if (epoll_fd < 0) {
    return 1;
  }

  int pipes[2];
  if (pipe(pipes) != 0) {
    close(epoll_fd);
    return 2;
  }

  struct epoll_event *event = malloc(sizeof(struct epoll_event));
  if (!event) {
    close(pipes[0]);
    close(pipes[1]);
    close(epoll_fd);
    return 3;
  }

  (void)epoll_ctl(epoll_fd, EPOLL_CTL_ADD, pipes[0], event + 1);
  free(event);
  close(pipes[0]);
  close(pipes[1]);
  close(epoll_fd);
  return 0;
}

static int test_bad_events(void) {
  int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
  struct epoll_event *events = malloc(sizeof(struct epoll_event));
  if (epoll_fd < 0 || !events) {
    free(events);
    if (epoll_fd >= 0) {
      close(epoll_fd);
    }
    return 1;
  }

  (void)epoll_wait(epoll_fd, events + 1, 1, 0);
  free(events);
  close(epoll_fd);
  return 0;
}

static int test_bad_signal_mask(void) {
  int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
  struct epoll_event *events = malloc(sizeof(struct epoll_event));
  sigset_t *signal_mask = malloc(sizeof(sigset_t));
  if (epoll_fd < 0 || !events || !signal_mask) {
    free(signal_mask);
    free(events);
    if (epoll_fd >= 0) {
      close(epoll_fd);
    }
    return 1;
  }

  (void)epoll_pwait(epoll_fd, events, 1, 0, signal_mask + 1);
  free(signal_mask);
  free(events);
  close(epoll_fd);
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    return 1;
  }
  if (!strcmp(argv[1], "good")) {
    return test_good();
  }
  if (!strcmp(argv[1], "bad-control")) {
    return test_bad_control();
  }
  if (!strcmp(argv[1], "bad-events")) {
    return test_bad_events();
  }
  if (!strcmp(argv[1], "bad-signal-mask")) {
    return test_bad_signal_mask();
  }
  return 2;
}
