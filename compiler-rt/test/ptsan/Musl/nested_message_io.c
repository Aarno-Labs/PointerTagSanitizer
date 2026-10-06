//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Exercise nested socket-message payloads through public musl APIs.
//
// RUN: %ptsan_musl_strict -o %t %s
// RUN: %run %t send-good
// RUN: %run %t recv-good
// RUN: %run %t recvmmsg-good
// RUN: not %run %t send-bad 2> %t.send
// RUN: FileCheck %s < %t.send
// RUN: not %run %t recv-bad 2> %t.recv
// RUN: FileCheck %s < %t.recv
// RUN: not %run %t recvmmsg-bad 2> %t.recvmmsg
// RUN: FileCheck %s < %t.recvmmsg
//
// CHECK: Found bad

#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

static int make_socket_pair(int sockets[2]) {
  return socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0, sockets);
}

static char *make_buffer(size_t size, char value) {
  char *buffer = malloc(size);
  if (buffer)
    memset(buffer, value, size);
  return buffer;
}

static int test_send(int bad) {
  int sockets[2];
  if (make_socket_pair(sockets))
    return 1;

  char *payload = make_buffer(4, 's');
  if (!payload)
    return 2;
  struct iovec vector = {
      .iov_base = payload,
      .iov_len = bad ? 8 : 4,
  };
  struct msghdr message = {
      .msg_iov = &vector,
      .msg_iovlen = 1,
  };

  ssize_t result = sendmsg(sockets[0], &message, 0);
  if (bad)
    return 0;

  char received[4];
  ssize_t received_size = read(sockets[1], received, sizeof(received));
  int ok = result == 4 && received_size == 4 &&
           !memcmp(received, payload, sizeof(received));
  free(payload);
  close(sockets[0]);
  close(sockets[1]);
  return ok ? 0 : 3;
}

static int test_recv(int bad) {
  int sockets[2];
  if (make_socket_pair(sockets))
    return 1;
  if (write(sockets[0], "received", 8) != 8)
    return 2;

  char *payload = make_buffer(4, 0);
  if (!payload)
    return 3;
  struct iovec vector = {
      .iov_base = payload,
      .iov_len = bad ? 8 : 4,
  };
  struct msghdr message = {
      .msg_iov = &vector,
      .msg_iovlen = 1,
  };

  ssize_t result = recvmsg(sockets[1], &message, 0);
  if (bad)
    return 0;

  int ok = result == 4 && !memcmp(payload, "rece", 4);
  free(payload);
  close(sockets[0]);
  close(sockets[1]);
  return ok ? 0 : 4;
}

static int test_recvmmsg(int bad) {
  int sockets[2];
  if (make_socket_pair(sockets))
    return 1;
  if (write(sockets[0], "firstmsg", 8) != 8)
    return 2;
  if (!bad && write(sockets[0], "second!!", 8) != 8)
    return 3;

  unsigned message_count = bad ? 1 : 2;
  char *payloads[2] = {
      make_buffer(4, 0),
      bad ? 0 : make_buffer(4, 0),
  };
  if (!payloads[0] || (!bad && !payloads[1]))
    return 4;

  struct iovec vectors[2] = {
      {.iov_base = payloads[0], .iov_len = bad ? 8 : 4},
      {.iov_base = payloads[1], .iov_len = 4},
  };
  struct mmsghdr messages[2] = {0};
  for (unsigned index = 0; index != message_count; ++index) {
    messages[index].msg_hdr.msg_iov = &vectors[index];
    messages[index].msg_hdr.msg_iovlen = 1;
  }

  int result = recvmmsg(sockets[1], messages, message_count, 0, 0);
  if (bad)
    return 0;

  int ok = result == 2 && messages[0].msg_len == 4 &&
           messages[1].msg_len == 4 && !memcmp(payloads[0], "firs", 4) &&
           !memcmp(payloads[1], "seco", 4);
  free(payloads[1]);
  free(payloads[0]);
  close(sockets[0]);
  close(sockets[1]);
  return ok ? 0 : 5;
}

int main(int argc, char **argv) {
  if (argc != 2)
    return 1;
  if (!strcmp(argv[1], "send-good"))
    return test_send(0);
  if (!strcmp(argv[1], "send-bad"))
    return test_send(1);
  if (!strcmp(argv[1], "recv-good"))
    return test_recv(0);
  if (!strcmp(argv[1], "recv-bad"))
    return test_recv(1);
  if (!strcmp(argv[1], "recvmmsg-good"))
    return test_recvmmsg(0);
  if (!strcmp(argv[1], "recvmmsg-bad"))
    return test_recvmmsg(1);
  return 2;
}
