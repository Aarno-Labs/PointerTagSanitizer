//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// pipe writes a fixed two-descriptor array.  poll reads and writes an array
// whose size is determined by nfds, through musl's cancellation-point path.
// No actual cancellation is requested here.
//
// RUN: %ptsan_musl_strict -o %t %s
// RUN: %run %t good
// RUN: not %run %t bad-pipe 2> %t.pipe
// RUN: FileCheck %s < %t.pipe
// RUN: not %run %t bad-poll 2> %t.poll
// RUN: FileCheck %s < %t.poll
// RUN: not %run %t null-poll 2> %t.null-poll
// RUN: FileCheck %s < %t.null-poll
//
// CHECK: Found bad

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int test_good(void) {
  int *fds = malloc(2 * sizeof(*fds));
  struct pollfd *descriptor = malloc(sizeof(*descriptor));
  if (!fds || !descriptor) {
    free(descriptor);
    free(fds);
    return 1;
  }
  if (pipe(fds) != 0) {
    free(descriptor);
    free(fds);
    return 2;
  }
  if (write(fds[1], "x", 1) != 1) {
    close(fds[0]);
    close(fds[1]);
    free(descriptor);
    free(fds);
    return 3;
  }

  descriptor->fd = fds[0];
  descriptor->events = POLLIN;
  descriptor->revents = 0;
  int result = poll(descriptor, 1, 0);
  errno = 0;
  int rejected_negative_count =
      getgroups(-1, (gid_t *)(fds + 2)) == -1 && errno == EINVAL;
  int ok =
      result == 1 && (descriptor->revents & POLLIN) && rejected_negative_count;
  close(fds[0]);
  close(fds[1]);
  free(descriptor);
  free(fds);
  return ok ? 0 : 4;
}

static int test_bad_pipe(void) {
  char *buffer = malloc(2 * sizeof(int) - 1);
  if (!buffer)
    return 1;
  (void)pipe((int *)buffer);
  free(buffer);
  return 0;
}

static int test_bad_poll(void) {
  char *buffer = calloc(1, sizeof(struct pollfd) - 1);
  if (!buffer)
    return 1;
  (void)poll((struct pollfd *)buffer, 1, 0);
  free(buffer);
  return 0;
}

static int test_null_poll(void) {
  (void)poll(NULL, 1, 0);
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 2)
    return 1;
  if (!strcmp(argv[1], "good"))
    return test_good();
  if (!strcmp(argv[1], "bad-pipe"))
    return test_bad_pipe();
  if (!strcmp(argv[1], "bad-poll"))
    return test_bad_poll();
  if (!strcmp(argv[1], "null-poll"))
    return test_null_poll();
  return 2;
}
