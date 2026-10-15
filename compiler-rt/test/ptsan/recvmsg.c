//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe 2> err.txt
// RUN: cat err.txt | FileCheck %s

// sendmsg/recvmsg/recvmmsg must strip ptsan tags from the msghdr's nested
// pointers (msg_iov and each iov_base) before the kernel uses them; a tagged
// address is non-canonical and the kernel returns EFAULT. They do it via shadow
// msghdr/iovec copies, so the caller's tagged fields must come back exactly as
// they went in, while kernel-written scalars (payload, msg_len) still flow back.
// Finally the buffer stays tagged for user accesses, so an OOB is reported.

// CHECK-NOT: FAIL:
// CHECK: recv: hello
// CHECK: mrecv: world
// CHECK: in_libc after recvmsg: 0
// CHECK: Found bad

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <threads.h>

extern thread_local int ptsan_in_libc;

static uint16_t tag_of(const void *p) { return (uint16_t)((uintptr_t)p >> 48); }

int __attribute__((noinline)) do_access(char *buf, int idx) {
  return buf[idx];
}

int main(void) {
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
    perror("socketpair");
    return 1;
  }

  // Send "hello" via sendmsg with tagged heap payload and iovec array.
  char *sbuf = strdup("hello");
  struct iovec *siov = (struct iovec *)malloc(sizeof(*siov));
  siov->iov_base = sbuf;
  siov->iov_len = 5;
  struct msghdr smsg;
  memset(&smsg, 0, sizeof(smsg));
  smsg.msg_iov = siov;
  smsg.msg_iovlen = 1;
  if (sendmsg(sv[1], &smsg, 0) != 5) {
    perror("sendmsg");
    return 1;
  }
  if (smsg.msg_iov != siov || siov->iov_base != sbuf || !tag_of(siov->iov_base)) {
    fprintf(stderr, "FAIL: sendmsg stripped the caller's tags\n");
    return 1;
  }

  char *buf = (char *)malloc(64);   // tagged under ptsan
  memset(buf, 0, 64);
  struct iovec iov = { .iov_base = buf, .iov_len = 64 };
  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;

  // If the interceptor failed to strip iov_base, the kernel write would EFAULT.
  ssize_t n = recvmsg(sv[0], &msg, 0);
  if (n < 0) {
    perror("recvmsg");
    return 1;
  }
  if (msg.msg_iov != &iov || iov.iov_base != buf || !tag_of(iov.iov_base)) {
    fprintf(stderr, "FAIL: recvmsg stripped the caller's tags\n");
    return 1;
  }
  buf[n] = '\0';
  fprintf(stderr, "recv: %s\n", buf);

  // recvmmsg: msg_len flows back, the caller's tagged fields survive.
  memcpy(sbuf, "world", 5);
  if (sendmsg(sv[1], &smsg, 0) != 5) {
    perror("sendmsg 2");
    return 1;
  }
  struct mmsghdr mm;
  memset(&mm, 0, sizeof(mm));
  iov.iov_base = buf;
  iov.iov_len = 64;
  mm.msg_hdr.msg_iov = &iov;
  mm.msg_hdr.msg_iovlen = 1;
  int r = recvmmsg(sv[0], &mm, 1, 0, NULL);
  if (r != 1 || mm.msg_len != 5) {
    fprintf(stderr, "FAIL: recvmmsg res/msg_len\n");
    return 1;
  }
  if (mm.msg_hdr.msg_iov != &iov || iov.iov_base != buf || !tag_of(iov.iov_base)) {
    fprintf(stderr, "FAIL: recvmmsg stripped the caller's tags\n");
    return 1;
  }
  buf[mm.msg_len] = '\0';
  fprintf(stderr, "mrecv: %s\n", buf);
  fprintf(stderr, "in_libc after recvmsg: %d\n", ptsan_in_libc);

  // buf is still tagged for user accesses, so this OOB must be reported.
  fprintf(stderr, "oob = %c\n", do_access(buf, 100));

  fprintf(stderr, "reached end without a report!\n");
  return 0;
}
