//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Some Linux syscall pointer arguments are conditionally ignored.  Supply a
// deliberately out-of-range pointer in those positions and verify musl's
// public APIs still succeed without a PTSan diagnostic.
//
// RUN: %ptsan_musl_strict -o %t %s
// RUN: %run %t

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static socklen_t *make_junk_length(socklen_t **allocation) {
  *allocation = malloc(sizeof(**allocation));
  return *allocation ? *allocation + 1 : 0;
}

static int test_absolute_sleep(void) {
  struct timespec request = {0, 0};
  struct timespec *allocation = malloc(sizeof(*allocation));
  if (!allocation)
    return 1;
  struct timespec *ignored_remaining = allocation + 1;
  int result = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &request,
                               ignored_remaining);
  free(allocation);
  return result == 0 ? 0 : 2;
}

static int test_accept(void) {
  int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (listener < 0)
    return 1;

  struct sockaddr_in address = {
      .sin_family = AF_INET,
      .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
  };
  if (bind(listener, (const struct sockaddr *)&address, sizeof(address)) != 0)
    goto fail_listener;

  socklen_t address_length = sizeof(address);
  if (getsockname(listener, (struct sockaddr *)&address, &address_length) !=
          0 ||
      listen(listener, 1) != 0)
    goto fail_listener;

  int client = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (client < 0)
    goto fail_listener;
  if (connect(client, (const struct sockaddr *)&address, address_length) != 0)
    goto fail_client;

  socklen_t *allocation;
  socklen_t *ignored_length = make_junk_length(&allocation);
  if (!ignored_length)
    goto fail_client;
  int accepted = accept(listener, 0, ignored_length);
  free(allocation);
  if (accepted < 0)
    goto fail_client;

  close(accepted);
  close(client);
  close(listener);
  return 0;

fail_client:
  close(client);
fail_listener:
  close(listener);
  return 2;
}

static int test_recvfrom(void) {
  int sockets[2];
  if (socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0, sockets) != 0)
    return 1;
  if (send(sockets[0], "x", 1, 0) != 1)
    goto fail;

  socklen_t *allocation;
  socklen_t *ignored_length = make_junk_length(&allocation);
  if (!ignored_length)
    goto fail;
  char byte;
  ssize_t received = recvfrom(sockets[1], &byte, 1, 0, 0, ignored_length);
  free(allocation);
  close(sockets[0]);
  close(sockets[1]);
  return received == 1 && byte == 'x' ? 0 : 2;

fail:
  close(sockets[0]);
  close(sockets[1]);
  return 3;
}

int main(void) {
  int result = test_absolute_sleep();
  if (result)
    return 10 + result;
  result = test_accept();
  if (result)
    return 20 + result;
  result = test_recvfrom();
  return result ? 30 + result : 0;
}
