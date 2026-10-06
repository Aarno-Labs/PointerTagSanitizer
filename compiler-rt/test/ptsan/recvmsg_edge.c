//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe 2> err.txt
// RUN: cat err.txt | FileCheck %s

// Edge cases for the shadow msghdr/iovec send/recv interceptors: NULL msghdr,
// NULL msgvec, tagged msg_iov with msg_iovlen == 0, kernel copy-back of
// msg_namelen/msg_controllen/msg_flags, and a partial recvmmsg fill that must
// leave the unfilled entry untouched.

// CHECK-NOT: BUG:
// CHECK-NOT: FAIL:
// CHECK: OK: recvmsg edge cases done

#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/uio.h>

static uint16_t tag_of(const void *p) { return (uint16_t)((uintptr_t)p >> 48); }

int main(void) {
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
    perror("socketpair");
    return 1;
  }

  // NULL msghdr forwards straight to libc and fails cleanly
  errno = 0;
  if (recvmsg(sv[0], NULL, MSG_DONTWAIT) != -1 || errno != EFAULT) {
    fprintf(stderr, "FAIL: NULL msghdr expected EFAULT (errno %d)\n", errno);
    return 1;
  }

  // NULL msgvec / vlen 0 must not crash or receive anything
  if (recvmmsg(sv[0], NULL, 0, MSG_DONTWAIT, NULL) > 0) {
    fprintf(stderr, "FAIL: NULL msgvec returned data\n");
    return 1;
  }

  if (write(sv[1], "ping", 4) != 4) {
    perror("write");
    return 1;
  }

  // tagged msg_iov with msg_iovlen == 0: COPY_CLEAR_IOVEC skips n == 0, so the
  // trailing clear_id must still strip the pointer; zero-length read leaves the
  // data queued and the caller's tag intact
  struct iovec *ziov = (struct iovec *)malloc(sizeof(*ziov));
  struct msghdr zm;
  memset(&zm, 0, sizeof(zm));
  zm.msg_iov = ziov;
  zm.msg_iovlen = 0;
  if (recvmsg(sv[0], &zm, MSG_DONTWAIT) != 0) {
    fprintf(stderr, "FAIL: zero-iov recvmsg\n");
    return 1;
  }
  if (zm.msg_iov != ziov || !tag_of(zm.msg_iov)) {
    fprintf(stderr, "FAIL: zero-iov leg lost the msg_iov tag\n");
    return 1;
  }

  // copy-back leg: pre-seed junk lengths/flags with tagged name/control
  // buffers; an AF_UNIX stream with no ancillary data sets all three to 0
  char *buf = (char *)malloc(64);
  char *namebuf = (char *)malloc(64);
  char *ctrlbuf = (char *)malloc(64);
  struct iovec iov = { .iov_base = buf, .iov_len = 64 };
  struct msghdr cm;
  memset(&cm, 0, sizeof(cm));
  cm.msg_name = namebuf;
  cm.msg_namelen = 64;
  cm.msg_control = ctrlbuf;
  cm.msg_controllen = 64;
  cm.msg_flags = 0x7f;
  cm.msg_iov = &iov;
  cm.msg_iovlen = 1;
  if (recvmsg(sv[0], &cm, MSG_DONTWAIT) != 4) {
    fprintf(stderr, "FAIL: copy-back recvmsg payload\n");
    return 1;
  }
  if (cm.msg_namelen != 0 || cm.msg_controllen != 0 || cm.msg_flags != 0) {
    fprintf(stderr, "FAIL: namelen/controllen/flags not copied back\n");
    return 1;
  }
  if (cm.msg_name != namebuf || !tag_of(cm.msg_name) ||
      cm.msg_control != ctrlbuf || !tag_of(cm.msg_control)) {
    fprintf(stderr, "FAIL: copy-back leg stripped pointer tags\n");
    return 1;
  }

  // partial recvmmsg fill: only one message queued, so entry 1 must come back
  // untouched (msg_len preserved, tags intact) and the copy-back stops at res
  if (write(sv[1], "pong", 4) != 4) {
    perror("write 2");
    return 1;
  }
  char *buf2 = (char *)malloc(64);
  struct iovec iov2 = { .iov_base = buf2, .iov_len = 64 };
  iov.iov_base = buf;
  iov.iov_len = 64;
  struct mmsghdr mv[2];
  memset(mv, 0, sizeof(mv));
  mv[0].msg_hdr.msg_iov = &iov;
  mv[0].msg_hdr.msg_iovlen = 1;
  mv[1].msg_hdr.msg_iov = &iov2;
  mv[1].msg_hdr.msg_iovlen = 1;
  mv[1].msg_len = 0xdeadu;
  int r = recvmmsg(sv[0], mv, 2, MSG_DONTWAIT, NULL);
  if (r != 1 || mv[0].msg_len != 4) {
    fprintf(stderr, "FAIL: partial recvmmsg r=%d len=%u\n", r, mv[0].msg_len);
    return 1;
  }
  if (mv[1].msg_len != 0xdeadu) {
    fprintf(stderr, "FAIL: unfilled entry msg_len modified\n");
    return 1;
  }
  if (mv[1].msg_hdr.msg_iov != &iov2 || iov2.iov_base != buf2 ||
      !tag_of(iov2.iov_base)) {
    fprintf(stderr, "FAIL: unfilled entry tags stripped\n");
    return 1;
  }

  fprintf(stderr, "OK: recvmsg edge cases done\n");
  return 0;
}
