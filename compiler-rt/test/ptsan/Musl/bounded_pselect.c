//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Exercise pselect descriptor sets and signal masks through the public musl API.
//
// RUN: %ptsan_musl_strict -o %t %s
// RUN: %run %t good
// RUN: not %run %t bad-fdset 2> %t.fdset
// RUN: FileCheck %s < %t.fdset
// RUN: not %run %t bad-signal-mask 2> %t.mask
// RUN: FileCheck %s < %t.mask
//
// CHECK: Found bad

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

static int test_good(void) {
  int pipes[2];
  if (pipe(pipes) != 0) {
    return 1;
  }

  fd_set *read_fds = malloc(sizeof(fd_set));
  sigset_t *signal_mask = malloc(sizeof(sigset_t));
  if (!read_fds || !signal_mask) {
    free(signal_mask);
    free(read_fds);
    close(pipes[0]);
    close(pipes[1]);
    return 2;
  }

  FD_ZERO(read_fds);
  FD_SET(pipes[0], read_fds);
  if (sigemptyset(signal_mask) != 0 || write(pipes[1], "x", 1) != 1) {
    goto fail;
  }

  struct timespec timeout = {0, 0};
  if (pselect(pipes[0] + 1, read_fds, NULL, NULL, &timeout, signal_mask) != 1) {
    goto fail;
  }

  // Linux sizes the raw descriptor bitmap from nfds. A single word is a
  // complete set for nfds == 1 even though libc's fd_set is larger.
  unsigned long compact_fds = 0;
  timeout = (struct timespec){0, 0};
  if (pselect(1, (fd_set *)&compact_fds, NULL, NULL, &timeout, NULL) != 0) {
    goto fail;
  }
  struct timeval select_timeout = {0, 0};
  if (select(1, (fd_set *)&compact_fds, NULL, NULL, &select_timeout) != 0) {
    goto fail;
  }

  fd_set *ignored_fds = read_fds + 1;
  timeout = (struct timespec){0, 0};
  if (pselect(0, ignored_fds, ignored_fds, ignored_fds, &timeout,
              signal_mask) != 0) {
    goto fail;
  }

  timeout = (struct timespec){0, 0};
  errno = 0;
  if (pselect(-1, ignored_fds, ignored_fds, ignored_fds, &timeout,
              signal_mask) != -1 ||
      errno != EINVAL) {
    goto fail;
  }

  free(signal_mask);
  free(read_fds);
  close(pipes[0]);
  close(pipes[1]);
  return 0;

fail:
  free(signal_mask);
  free(read_fds);
  close(pipes[0]);
  close(pipes[1]);
  return 3;
}

static int test_bad_fdset(void) {
  fd_set *read_fds = malloc(sizeof(fd_set));
  if (!read_fds) {
    return 1;
  }

  struct timespec timeout = {0, 0};
  (void)pselect(1, read_fds + 1, NULL, NULL, &timeout, NULL);
  free(read_fds);
  return 0;
}

static int test_bad_signal_mask(void) {
  void *signal_mask = calloc(1, sizeof(sigset_t) - 1);
  if (!signal_mask) {
    return 1;
  }

  struct timespec timeout = {0, 0};
  (void)pselect(0, NULL, NULL, NULL, &timeout, (const sigset_t *)signal_mask);
  free(signal_mask);
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    return 1;
  }
  if (!strcmp(argv[1], "good")) {
    return test_good();
  }
  if (!strcmp(argv[1], "bad-fdset")) {
    return test_bad_fdset();
  }
  if (!strcmp(argv[1], "bad-signal-mask")) {
    return test_bad_signal_mask();
  }
  return 2;
}
