//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict -Wall -Wextra -Werror %s -o %t
// RUN: %run %t

#define _GNU_SOURCE
#include <assert.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(void) {
  alarm(10);
  int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  assert(listener >= 0);
  struct sockaddr_in address = {
      .sin_family = AF_INET,
      .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
  };
  assert(bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0);
  assert(listen(listener, 1) == 0);
  socklen_t size = sizeof(address);
  assert(getsockname(listener, (struct sockaddr *)&address, &size) == 0);

  int sender = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  assert(sender >= 0);
  assert(connect(sender, (struct sockaddr *)&address, sizeof(address)) == 0);
  int receiver = accept(listener, NULL, NULL);
  assert(receiver >= 0);
  int enabled = 1;
  assert(setsockopt(receiver, IPPROTO_TCP, TCP_INQ, &enabled, sizeof(enabled)) == 0);

  const char data[] = "abcdefgh";
  assert(send(sender, data, 8, 0) == 8);
  char peek[8];
  // Ensure all eight bytes are queued before inspecting the remaining count.
  assert(recv(receiver, peek, sizeof(peek), MSG_PEEK | MSG_WAITALL) == 8);
  char received[4];
  union {
    struct cmsghdr alignment;
    unsigned char bytes[CMSG_SPACE(sizeof(int))];
  } control = {0};
  struct iovec iov = {.iov_base = received, .iov_len = sizeof(received)};
  struct msghdr message = {
      .msg_iov = &iov,
      .msg_iovlen = 1,
      .msg_control = control.bytes,
      .msg_controllen = sizeof(control.bytes),
  };
  assert(recvmsg(receiver, &message, MSG_WAITALL) == 4);
  assert(memcmp(received, data, 4) == 0);
  assert(!(message.msg_flags & MSG_CTRUNC));
  struct cmsghdr *header = CMSG_FIRSTHDR(&message);
  assert(header != NULL);
  assert(header->cmsg_level == IPPROTO_TCP && header->cmsg_type == TCP_CM_INQ);
  assert(header->cmsg_len == CMSG_LEN(sizeof(int)));
  int remaining = -1;
  memcpy(&remaining, CMSG_DATA(header), sizeof(remaining));
  assert(remaining == 4);

  assert(close(receiver) == 0);
  assert(close(sender) == 0);
  assert(close(listener) == 0);
  return 0;
}
